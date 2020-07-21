/*
This file is part of cpp-ethereum.

cpp-ethereum is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

cpp-ethereum is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <time.h>
#include <functional>
#include <libethash-legacy/legacy_ethash.h>
#include <libdevcore/Worker.h>
#include <libethcore/ProgPowAux.h>
#include <libethcore/Miner.h>
#include "libethash-legacy/internal.h"
#include <libprogpow/ProgPow.h>
#include <cuda.h>
#include "ProgPowCUDAMiner_cuda.h"
#include <libethash-cuda/CUDAMiner.h>

namespace dev
{
namespace eth
{

class CUMinerFactory
{
    public:
        Miner* GetCUMinerIntance(unsigned _index, PowType _powType, CUSettings _settings, DeviceDescriptor& _device);
};

class ProgPowCUDAMiner: public CUDAMiner
{

public:
	ProgPowCUDAMiner(unsigned _index, CUSettings _settings, DeviceDescriptor& _device);
	~ProgPowCUDAMiner() override;
	static unsigned instances()
	{
		return s_numInstances > 0 ? s_numInstances : 1;
	}
	static unsigned getNumDevices();
	static void listDevices();
	static void setParallelHash(unsigned _parallelHash);
	static bool configureGPU(
		unsigned _blockSize,
		unsigned _gridSize,
		unsigned _numStreams,
		unsigned _scheduleFlag,
		uint64_t _currentBlock,
		unsigned _dagLoadMode,
		unsigned _dagCreateDevice,
		bool _noeval,
		bool _exit
		);
	static void setNumInstances(unsigned _instances);
	static void setDevices(const vector<unsigned>& _devices, unsigned _selectedDeviceCount);
	static bool cuda_configureGPU(
		size_t numDevices,
		const vector<int>& _devices,
		unsigned _blockSize,
		unsigned _gridSize,
		unsigned _numStreams,
		unsigned _scheduleFlag,
		uint64_t _currentBlock,
		bool _noeval
		);

	static void cuda_setParallelHash(unsigned _parallelHash);

	bool cuda_init(
		size_t numDevices,
		ethash_light_t _light,
		uint8_t const* _lightData,
		uint64_t _lightSize,
		unsigned _deviceId,
		bool _cpyToHost,
		uint8_t * &hostDAG,
		unsigned dagCreateDevice);

	void search(
		uint8_t const* header,
		uint64_t target,
		bool _ethStratum,
		uint64_t _startN,
		const dev::eth::WorkPackage& w);

	/* -- default values -- */
	/// Default value of the block size. Also known as workgroup size.
	static unsigned const c_defaultBlockSize;
	/// Default value of the grid size
	static unsigned const c_defaultGridSize;
	// default number of CUDA streams
	static unsigned const c_defaultNumStreams;

protected:
	void kick_miner() override;

private:
	atomic<bool> m_new_work = {false};

	void workLoop() override;

	bool init(int epoch);

	progpow_hash32_t m_current_header;
	uint64_t m_current_target;
	uint64_t m_current_nonce;
	uint64_t m_starting_nonce;
	uint64_t m_current_index;

	///Constants on GPU
	progpow_hash64_t* m_dag = nullptr;
	std::vector<progpow_hash64_t*> m_light;
	uint32_t m_dag_elms = -1;
	uint32_t m_device_num;

	CUmodule m_module;
	CUfunction m_kernel;
	volatile search_results** m_search_buf;
	cudaStream_t  * m_streams;

	/// The local work size for the search
	static unsigned s_blockSize;
	/// The initial global work size for the searches
	static unsigned s_gridSize;
	/// The number of CUDA streams
	static unsigned s_numStreams;
	/// CUDA schedule flag
	static unsigned s_scheduleFlag;

	static unsigned m_parallelHash;

	static unsigned s_numInstances;
	static vector<int> s_devices;

	static bool s_noeval;

	void compileKernel(uint64_t block_number, uint64_t dag_words);

};


}
}
