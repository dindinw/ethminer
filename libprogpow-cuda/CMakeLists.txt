# A custom command and target to turn the CUDA kernel into a byte array header
# The normal build depends on it properly and if the kernel file is changed, then
# a rebuild of libprogpow-cuda should be triggered

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ProgPowCUDAMiner_kernel.h
	COMMAND ${CMAKE_COMMAND} ARGS
	-DBIN2H_SOURCE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/ProgPowCUDAMiner_kernel.cu"
	-DBIN2H_VARIABLE_NAME=ProgPowCUDAMiner_kernel
	-DBIN2H_HEADER_FILE="${CMAKE_CURRENT_BINARY_DIR}/ProgPowCUDAMiner_kernel.h"
	-P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/bin2h.cmake"
	COMMENT "Generating CUDA Kernel Byte Array"
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ProgPowCUDAMiner_kernel.cu
)
add_custom_target(cuda_kernel DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/ProgPowCUDAMiner_kernel.h ${CMAKE_CURRENT_SOURCE_DIR}/ProgPowCUDAMiner_kernel.cu)

find_package(CUDA REQUIRED)

set(CUDA_NVCC_FLAGS ${CUDA_NVCC_FLAGS};--ptxas-options=-v;-lineinfo)

if (NOT MSVC)
	list(APPEND CUDA_NVCC_FLAGS "--disable-warnings")
endif()

list(APPEND CUDA_NVCC_FLAGS_RELEASE -O3)
list(APPEND CUDA_NVCC_FLAGS_DEBUG -G)

if(COMPUTE AND (COMPUTE GREATER 0))
	list(APPEND CUDA_NVCC_FLAGS "-gencode arch=compute_${COMPUTE},code=sm_${COMPUTE}")
else()
	set(
		CUDA_NVCC_FLAGS
		${CUDA_NVCC_FLAGS}
		"-gencode arch=compute_61,code=sm_61"
		"-gencode arch=compute_75,code=sm_75"
	)
endif()

file(GLOB sources ProgPowCUDAMiner.cpp ProgPowCUDAMiner_cuda.cu)
file(GLOB headers ProgPowCUDAMiner.h ProgPowCUDAMiner_cuda.h ${CMAKE_CURRENT_BINARY_DIR}/ProgPowCUDAMiner_kernel.h)

# Cmake doesn't handle nvrtc automatically
find_library(CUDA_nvrtc_LIBRARY nvrtc "${CUDA_TOOLKIT_ROOT_DIR}/lib64" "${CUDA_TOOLKIT_ROOT_DIR}/lib/x64")
find_library(CUDA_cuda_LIBRARY cuda "${CUDA_TOOLKIT_ROOT_DIR}/lib64" "${CUDA_TOOLKIT_ROOT_DIR}/lib/x64")
MESSAGE(STATUS "===========================${CUDA_cuda_LIBRARY} ${CUDA_nvrtc_LIBRARY}")
cuda_add_library(progpow-cuda STATIC ${sources} ${headers})
add_dependencies(progpow-cuda cuda_kernel)
target_link_libraries(progpow-cuda ethcore ethash ethash-legacy Boost::thread progpow)
target_link_libraries(progpow-cuda ${CUDA_nvrtc_LIBRARY} ${CUDA_cuda_LIBRARY})
target_include_directories(progpow-cuda PUBLIC ${CUDA_INCLUDE_DIRS})
target_include_directories(progpow-cuda PRIVATE .. ${CMAKE_CURRENT_BINARY_DIR})