#pragma once
#include <cstddef>
namespace sp{
// G (CUDA) scope: evaluate candidate distances on the GPU. The logical candidate set is always
// built and fully retained on the host; the GPU only fills distance values.
// Returns false when the binary was built without CUDA or no device is usable.
bool cudaAvailable();
// A and B are 3*n SoA coordinate arrays. Fills d[i] with the Euclidean distance.
// Returns 0 on success, a non-zero CUDA error code otherwise. Distances are bit-identical to the
// host norm() because the kernel uses the same expression order with FMA contraction disabled.
int cudaCandidateDistance(const double*A,const double*B,std::size_t n,double*d);
}
