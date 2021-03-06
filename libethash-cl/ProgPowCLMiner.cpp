/// OpenCL Ethash miner implementation.
///
/// @file
/// @copyright GNU General Public License
//

#include <libethcore/Farm.h>
#include "ProgPowCLMiner.h"
#include "progpow.h"

using namespace dev;
using namespace eth;

#define LOG2_MAX_MINERS 5u
unsigned ProgPowCLMiner::s_workgroupSize = 256; //defaultLocalWorkSize
unsigned ProgPowCLMiner::s_initialGlobalWorkSize =  2048 * 256 ; // defaultGlobalWorkSizeMultiplier * defaultLocalWorkSize

constexpr size_t c_maxSearchResults = 1;
std::chrono::high_resolution_clock::time_point workSwitchStart;

struct CLSwitchChannel: public LogChannel
{
    static const char* name() { return EthOrange " cl"; }
    static const int verbosity = 6;
    static const bool debug = false;
};
#define clswitchlog clog(CLSwitchChannel)

ProgPowCLMiner::ProgPowCLMiner(unsigned _index, CLSettings _settings, DeviceDescriptor& _device)
    : CLMiner(_index, _settings,_device)
{
    DEV_BUILD_LOG_PROGRAMFLOW(cllog, "cl-" << m_index << " ProgPowCLMiner::EthashCLMiner() called");
}

ProgPowCLMiner::~ProgPowCLMiner()
{
    DEV_BUILD_LOG_PROGRAMFLOW(cllog, "cl-" << m_index << " ProgPowCLMiner::~EthashCLMiner() begin");
    stopWorking();
    kick_miner();
    DEV_BUILD_LOG_PROGRAMFLOW(cllog, "cl-" << m_index << " ProgPowCLMiner::~EthashCLMiner() end");
}

uint64_t get_start_nonce(uint _index)
{
    // Each GPU is given a non-overlapping 2^40 range to search
    return Farm::f().get_nonce_scrambler() + ((uint64_t) _index << 40);
}

void ProgPowCLMiner::workLoop()
{
    // Memory for zero-ing buffers. Cannot be static because crashes on macOS.
    uint32_t const c_zero = 0;

    uint64_t startNonce = 0;

    // The work package currently processed by GPU.
    WorkPackage current;
    current.header = h256{1u};
    uint64_t old_period_seed = -1;

    if (!initDevice())
        return;

    try {
        while (!shouldStop())
        {
            const WorkPackage w = work();
            uint64_t period_seed = w.block / PROGPOW_PERIOD;

            if (current.header != w.header || current.epoch != w.epoch || old_period_seed != period_seed)
            {
                // New work received. Update GPU data.
                if (!w)
                {
                    cllog << "No work. Pause for 3 s.";
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    continue;
                }


                DEV_BUILD_LOG_PROGRAMFLOW(cllog, "cl-" << m_index << " New work: header=" << w.header << ", target=" << w.boundary.hex());

                if (current.epoch != w.epoch || old_period_seed != period_seed)
                {
                    if (s_dagLoadMode == DAG_LOAD_MODE_SEQUENTIAL)
                    {
                        while (s_dagLoadIndex < m_index)
                            this_thread::sleep_for(chrono::seconds(1));
                        ++s_dagLoadIndex;
                    }

                    cllog << "New epoch " << w.epoch << "/ period " << period_seed;
                    init(w.epoch, w.block, current.epoch != w.epoch, old_period_seed != period_seed);
                }

                // Upper 64 bits of the boundary.
                const uint64_t target = (uint64_t)(u64)((u256)w.boundary >> 192);
                assert(target > 0);

                // Update header constant buffer.
                m_queue.enqueueWriteBuffer(m_header, CL_FALSE, 0, w.header.size, w.header.data());
                m_queue.enqueueWriteBuffer(m_searchBuffer, CL_FALSE, 0, sizeof(c_zero), &c_zero);

                m_searchKernel.setArg(0, m_searchBuffer);  // Supply output buffer to kernel.
                m_searchKernel.setArg(4, target);

                // FIXME: This logic should be move out of here.
                if (w.exSizeBytes > 0)
                {
                    // This can support up to 2^c_log2MaxMiners devices.
                    startNonce = w.startNonce | ((uint64_t)m_index << (64 - LOG2_MAX_MINERS - w.exSizeBytes));
                }
                else
                    startNonce = get_start_nonce(m_index);

                clswitchlog << "Switch time"
                            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - workSwitchStart).count()
                            << "ms.";
            }

            // Read results.
            // TODO: could use pinned host pointer instead.
            uint32_t results[c_maxSearchResults + 1];
            m_queue.enqueueReadBuffer(m_searchBuffer, CL_TRUE, 0, sizeof(results), &results);

            uint64_t nonce = 0;
            if (results[0] > 0)
            {
                // Ignore results except the first one.
                nonce = current.startNonce + results[1];
                // Reset search buffer if any solution found.
                m_queue.enqueueWriteBuffer(m_searchBuffer, CL_FALSE, 0, sizeof(c_zero), &c_zero);
            }

            // Run the kernel.
            m_searchKernel.setArg(3, startNonce);
            m_queue.enqueueNDRangeKernel(m_searchKernel, cl::NullRange, m_globalWorkSize, m_workgroupSize);

            // Report results while the kernel is running.
            // It takes some time because ethash must be re-evaluated on CPU.
            if (nonce != 0) {
                Result r = EthashAux::eval(current.epoch, current.header, nonce);
                if (r.value < current.boundary)
                    Farm::f().submitProof(Solution{nonce, r.mixHash, current, std::chrono::steady_clock::now(), m_index});
                else {
                    cwarn << "FAILURE: GPU gave incorrect result!";
                }
            }

            old_period_seed = period_seed;

            current = w;        // kernel now processing newest work
            current.startNonce = startNonce;
            // Increase start nonce for following kernel execution.
            startNonce += m_globalWorkSize;

            // Report hash count
            updateHashRate(m_globalWorkSize, 1);


            // Make sure the last buffer write has finished --
            // it reads local variable.
            m_queue.finish();
        }
        m_queue.finish();
    }
    catch (cl::Error const& _e)
    {
        string _what = ethCLErrorHelper("OpenCL Error", _e);
        cwarn << _what;
        throw std::runtime_error(_what);
    }
}

bool ProgPowCLMiner::init(int epoch, uint64_t block_number, bool new_epoch, bool new_period)
{
    cllog << "ProgPow Miner init, epoch= " << epoch
          << " block_number=" << block_number
          << " new_epoch=" << new_epoch
          << " new_period=" << new_period;

    assert(new_epoch || new_period);

    ProgPowAux::LightType light = ProgPowAux::light(epoch);

    try
    {
        char options[256] = {0};
        int computeCapability = 0;
#ifndef __clang__

        // Nvidia
        if (!m_deviceDescriptor.clNvCompute.empty())
        {
            computeCapability =
                m_deviceDescriptor.clNvComputeMajor * 10 + m_deviceDescriptor.clNvComputeMinor;
            int maxregs = computeCapability >= 35 ? 72 : 63;
            sprintf(options, "-cl-nv-maxrregcount=%d", maxregs);
        }

#endif

        // create context
        m_context = cl::Context(vector<cl::Device>(&m_device, &m_device + 1));
        m_queue = cl::CommandQueue(m_context, m_device);

        // make sure that global work size is evenly divisible by the local workgroup size
        m_workgroupSize = s_workgroupSize;
        m_globalWorkSize = s_initialGlobalWorkSize;
        if (m_globalWorkSize % m_workgroupSize != 0)
            m_globalWorkSize = ((m_globalWorkSize / m_workgroupSize) + 1) * m_workgroupSize;

        uint64_t dagBytes = ethash_get_datasize(light->light->block_number);
        uint32_t dagElms = (unsigned)(dagBytes / (PROGPOW_LANES * PROGPOW_DAG_LOADS * 4));
        uint32_t lightWords = (unsigned)(light->data().size() / sizeof(node));

        // patch source code
        // note: The kernels here are simply compiled version of the respective .cl kernels
        // into a byte array by bin2h.cmake. There is no need to load the file by hand in runtime
        // See libethash-cl/CMakeLists.txt: add_custom_command()
        cllog << "OpenCL ProgPOW kernel";
        std::string code = ProgPow::getKern(block_number, ProgPow::KERNEL_CL);
        code += string(progpow_cl, progpow_cl + sizeof(progpow_cl));

        addDefinition(code, "GROUP_SIZE", m_workgroupSize);
        addDefinition(code, "PROGPOW_DAG_BYTES", dagBytes);
        addDefinition(code, "PROGPOW_DAG_ELEMENTS", dagElms);
        addDefinition(code, "LIGHT_WORDS", lightWords);
        addDefinition(code, "MAX_OUTPUTS", c_maxSearchResults);
        addDefinition(code, "PLATFORM", m_deviceDescriptor.clPlatformId);
        addDefinition(code, "COMPUTE", computeCapability);

        ofstream out;
        out.open("kernel.cl");
        out << code;
        out.close();

        // create miner OpenCL program
        cl::Program::Sources sources{{code.data(), code.size()}};
        cl::Program program(m_context, sources);
        try
        {
            program.build({m_device}, options);
            cllog << "Build info:" << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(m_device);
        }
        catch (cl::Error const&)
        {
            cwarn << "Build info:" << program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(m_device);
            return false;
        }

        //check whether the current dag fits in memory everytime we recreate the DAG
        cl_ulong result = 0;
        m_device.getInfo(CL_DEVICE_GLOBAL_MEM_SIZE, &result);
        if (result < dagBytes)
        {
            cnote <<
                  "OpenCL device " << m_device.getInfo<CL_DEVICE_NAME>()
                  << " has insufficient GPU memory." << result <<
                  " bytes of memory found < " << dagBytes << " bytes of memory required";
            return false;
        }

        // create buffer for dag
        try
        {
            cllog << "Creating light cache buffer, size" << light->data().size();
            m_light = cl::Buffer(m_context, CL_MEM_READ_ONLY, light->data().size());
            cllog << "Creating DAG buffer, size" << dagBytes;
            m_dag = cl::Buffer(m_context, CL_MEM_READ_ONLY, dagBytes);
            cllog << "Loading kernels";
            m_searchKernel = cl::Kernel(program, "ethash_search");
            m_dagKernel = cl::Kernel(program, "ethash_calculate_dag_item");
            cllog << "Writing light cache buffer";
            m_queue.enqueueWriteBuffer(m_light, CL_TRUE, 0, light->data().size(), light->data().data());
        }
        catch (cl::Error const& err)
        {
            cwarn << ethCLErrorHelper("Creating DAG buffer failed", err);
            return false;
        }
        // create buffer for header
        cllog << "Creating buffer for header.";
        m_header = cl::Buffer(m_context, CL_MEM_READ_ONLY, 32);

        m_searchKernel.setArg(1, m_header);
        m_searchKernel.setArg(2, m_dag);
        m_searchKernel.setArg(5, 0);

        if (!new_epoch)
            return true;

        // create mining buffers
        cllog << "Creating mining buffer";
        m_searchBuffer = cl::Buffer(m_context, CL_MEM_WRITE_ONLY, (c_maxSearchResults + 1) * sizeof(uint32_t));

        uint32_t const work = (uint32_t)(dagBytes / sizeof(node));
        uint32_t fullRuns = work / m_globalWorkSize;
        uint32_t const restWork = work % m_globalWorkSize;
        if (restWork > 0) fullRuns++;

        m_dagKernel.setArg(1, m_light);
        m_dagKernel.setArg(2, m_dag);
        m_dagKernel.setArg(3, ~0u);

        auto startDAG = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < fullRuns; i++)
        {
            m_dagKernel.setArg(0, i * m_globalWorkSize);
            m_queue.enqueueNDRangeKernel(m_dagKernel, cl::NullRange, m_globalWorkSize, m_workgroupSize);
            m_queue.finish();
        }
        auto endDAG = std::chrono::steady_clock::now();

        auto dagTime = std::chrono::duration_cast<std::chrono::milliseconds>(endDAG-startDAG);
        float gb = (float)dagBytes / (1024 * 1024 * 1024);
        cnote << gb << " GB of DAG data generated in" << dagTime.count() << "ms.";
    }
    catch (cl::Error const& err)
    {
        cwarn << ethCLErrorHelper("OpenCL init failed", err);
        return false;
    }
    return true;
}

