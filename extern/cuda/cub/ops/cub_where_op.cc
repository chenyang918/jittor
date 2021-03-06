// ***************************************************************
// Copyright (c) 2020 Jittor. Authors: 
//     Xiangli Li <1905692338@qq.com>
//     Dun Liang <randonlang@gmail.com>. 
// All Rights Reserved.
// This file is subject to the terms and conditions defined in
// file 'LICENSE.txt', which is part of this source code package.
// ***************************************************************
#include "var.h"
#include "cub_where_op.h"
#ifdef JIT_cuda
#include "executor.h"
#include <assert.h>
#include <executor.h>
#include <cub/cub.cuh>
#include <thrust/device_vector.h>
#include <thrust/transform.h>
#endif

namespace jittor {

#ifndef JIT
CubWhereOp::CubWhereOp(Var* cond, NanoString dtype) : cond(cond) {
    flags.set(NodeFlags::_cpu);
    flags.set(NodeFlags::_cuda);
    flags.set(NodeFlags::_vary_shape);
    auto ndim = cond->shape.size();
    outs.reset(new Var*[ndim]);
    for (uint i=0; i<ndim; i++)
        outs[i] = create_output(nullptr, dtype);
}

void CubWhereOp::infer_shape() {
    auto ndim = cond->shape.size();
    auto num = cond->num;
    if (num>0) num = -num;
    for (uint i=0; i<ndim; i++)
        outs[i]->set_shape({num});
}

void CubWhereOp::jit_prepare(JK& jk) {
    jk << _CS("[Ti:") << cond->dtype();
    jk << _CS("][To:") << outs[0]->dtype();
    jk << _CS("][NDIM=") << JK::hex1(cond->shape.size());
    jk << ']';
}

#else // JIT
#ifdef JIT_cuda

template<typename T>
struct NonZeroOp
{
    __host__ __device__ __forceinline__ bool operator()(const T& a) const {
      return (a!=T(0));
    }
};

template<typename T>
struct ConvertOp 
{   
    const T div;
    const T dim_size;
    ConvertOp(T _div,T dim_size): div(_div),dim_size(dim_size){} 
    __host__ __device__ __forceinline__ T operator()(const T& val) const {
        return (val/div) % dim_size;
    }
};

__global__ static void where_kernel(
    int n, 
    To* input
    @for(i, 0, NDIM, 1, ,index_t shape_@i, To* out_@i)
) {
    int tid = threadIdx.x + blockIdx.x * blockDim.x;
    int tnum = gridDim.x * blockDim.x;
    for (index_t i=tid; i<n; i+=tnum) {
        index_t x = input[i];
        @for(j, NDIM-1, 0, -1, 
            index_t i@j = x % shape_@j;
            out_@j[i] = i@j;
            x /= shape_@j;
        )
        out_0[i] = x;
        (void)shape_0;
    }
}

void CubWhereOp::jit_run(){
    int N = cond->num;
    size_t temp_storage_bytes=0;
    size_t num_nonzeros_allocation;
    auto num_nonzeros = exe.allocator->alloc(sizeof(int), num_nonzeros_allocation);
    cub::TransformInputIterator<bool, NonZeroOp<Ti>, Ti*> itr(cond->ptr<Ti>(), NonZeroOp<Ti>());
    cub::DeviceReduce::Sum(nullptr, temp_storage_bytes, itr, (int *)num_nonzeros, N);
    
    size_t temp_storage_allocation;
    auto temp_storage = exe.allocator->alloc(temp_storage_bytes, temp_storage_allocation);

    cub::DeviceReduce::Sum(temp_storage, temp_storage_bytes, itr, (int *)num_nonzeros, N);
    exe.allocator->free(temp_storage, temp_storage_bytes, temp_storage_allocation);

    int num_nonzeros_h;
    checkCudaErrors(cudaMemcpy(&num_nonzeros_h, num_nonzeros, sizeof(int), cudaMemcpyDeviceToHost));
    
    To* out_temp = outs[0]->ptr<To>();

    @for(i, 0, NDIM, outs[@i]->set_shape({num_nonzeros_h});)

    cub::CountingInputIterator<To> counting_itr(0);
    temp_storage_bytes = 0;
    cub::DeviceSelect::Flagged(nullptr, temp_storage_bytes, counting_itr, itr,out_temp, (int*)num_nonzeros, N);
    temp_storage = exe.allocator->alloc(temp_storage_bytes, temp_storage_allocation);
    cub::DeviceSelect::Flagged(temp_storage, temp_storage_bytes, counting_itr, itr,out_temp, (int*)num_nonzeros, N);
    exe.allocator->free(temp_storage, temp_storage_bytes, temp_storage_allocation);

    if (num_nonzeros_h > 0 && NDIM > 1) {
        int thread_num = std::min(1024, num_nonzeros_h);
        int block_num = std::max(1, num_nonzeros_h/1024);
        where_kernel<<<block_num, thread_num>>>(
            num_nonzeros_h, 
            out_temp
            @for(i, 0, NDIM, 1, , cond->shape[@i], outs[@i]->ptr<To>())
        );
    }
    exe.allocator->free(num_nonzeros, sizeof(int), num_nonzeros_allocation);
    
}
#endif
#endif // JIT

} // jittor
