#include "cuda_candidates.hpp"
#include <cuda_runtime.h>
#include <cmath>
namespace sp{
__global__ void kdistance(const double*A,const double*B,unsigned long long n,double*d){
unsigned long long i=static_cast<unsigned long long>(blockIdx.x)*blockDim.x+threadIdx.x;
if(i>=n)return;
double dx=B[3*i]-A[3*i],dy=B[3*i+1]-A[3*i+1],dz=B[3*i+2]-A[3*i+2];
d[i]=sqrt(dx*dx+dy*dy+dz*dz);
}
bool cudaAvailable(){
int n=0;
if(cudaGetDeviceCount(&n)!=cudaSuccess)return false;
return n>0;
}
int cudaCandidateDistance(const double*A,const double*B,std::size_t n,double*d){
if(n==0)return 0;
double*da=nullptr;
double*db=nullptr;
double*dd=nullptr;
cudaError_t e=cudaMalloc(reinterpret_cast<void**>(&da),n*3*sizeof(double));
if(!e)e=cudaMalloc(reinterpret_cast<void**>(&db),n*3*sizeof(double));
if(!e)e=cudaMalloc(reinterpret_cast<void**>(&dd),n*sizeof(double));
if(!e)e=cudaMemcpy(da,A,n*3*sizeof(double),cudaMemcpyHostToDevice);
if(!e)e=cudaMemcpy(db,B,n*3*sizeof(double),cudaMemcpyHostToDevice);
if(!e){
unsigned threads=256;
unsigned blocks=static_cast<unsigned>((n+threads-1)/threads);
kdistance<<<blocks,threads>>>(da,db,static_cast<unsigned long long>(n),dd);
e=cudaGetLastError();
}
if(!e)e=cudaDeviceSynchronize();
if(!e)e=cudaMemcpy(d,dd,n*sizeof(double),cudaMemcpyDeviceToHost);
if(da)cudaFree(da);
if(db)cudaFree(db);
if(dd)cudaFree(dd);
return int(e);
}
}
