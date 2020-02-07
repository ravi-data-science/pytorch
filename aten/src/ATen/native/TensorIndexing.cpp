#include <ATen/native/TensorIndexing.h>

#include <c10/util/Exception.h>

namespace at {
namespace indexing {

const EllipsisIndexType Ellipsis = EllipsisIndexType();
TensorIndex tensor_index_tmp = TensorIndex(None);

std::ostream& operator<<(std::ostream& stream, const Slice& slice) {
  stream << slice.start() << ":" << slice.stop() << ":" << slice.step();
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const TensorIndex& tensor_index) {
  if (tensor_index.is_none()) {
    stream << "None";
  } else if (tensor_index.is_ellipsis()) {
    stream << "...";
  } else if (tensor_index.is_integer()) {
    stream << tensor_index.integer();
  } else if (tensor_index.is_boolean()) {
    stream << std::boolalpha << tensor_index.boolean();
  } else if (tensor_index.is_slice()) {
    stream << tensor_index.slice();
  } else if (tensor_index.is_tensor()) {
    stream << tensor_index.tensor();
  }
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const std::vector<TensorIndex>& tensor_indices) {
  stream << "(";
  for (size_t i = 0; i < tensor_indices.size(); i++) {
    stream << tensor_indices[i];
    if (i < tensor_indices.size() - 1) stream << ", ";
  }
  stream << ")";
  return stream;
}

// This mirrors `count_specified_dimensions` in torch/csrc/autograd/python_variable_indexing.cpp
int64_t count_specified_dimensions(ArrayRef<TensorIndex> indices) {
  // Count the number of indexed dimensions (everything but ellipsis and None)
  int64_t count = 0;
  size_t size = indices.size();
  for (auto& obj : indices) {
    if (obj.is_tensor()) {
      auto& tensor = obj.tensor();
      if (tensor.scalar_type() == kByte || tensor.scalar_type() == kBool) {
        count += tensor.dim();
      } else {
        count++;
      }
    } else if (!obj.is_none() && !obj.is_ellipsis() && !obj.is_boolean()) {
      count++;
    }
  }
  return count;
}

// This mirrors `applySlicing` in torch/csrc/autograd/python_variable_indexing.cpp
Tensor applySlicing(const Tensor& self, ArrayRef<TensorIndex> indices, std::vector<Tensor>& outIndices) {
  int64_t size = indices.size();
  int64_t dim = 0;
  int64_t specified_dims = count_specified_dimensions(indices);

  TORCH_CHECK_INDEX(specified_dims <= self.dim(), "too many indices for tensor of dimension ", (int)self.dim());

  Tensor result = self;
  for (int64_t i = 0; i < size; i++) {
    auto& obj = indices[i];
    result = handleDimInMultiDimIndexing(
      /*prev_dim_result=*/result,
      /*original_tensor=*/self,
      /*index=*/obj,
      /*dim=*/&dim,
      /*specified_dims=*/&specified_dims,
      /*real_dim=*/i,
      /*outIndices=*/outIndices,
      /*is_tracing=*/false);
  }
  return result;
}

// This mirrors `THPVariable_getitem` in torch/csrc/autograd/python_variable_indexing.cpp
Tensor get_item(const Tensor& self, ArrayRef<TensorIndex> indices) {
  OptionalDeviceGuard device_guard(device_of(self));

  // handle simple types: integers, slices, ellipsis
  if (indices.size() == 1) {
    const TensorIndex& index = indices[0];
    if (!index.is_boolean() && !index.is_tensor()) {
      return handleSimpleTypesInSingleDimIndexingGet(self, index, /*is_tracing=*/false);
    }
  }

  std::vector<Tensor> tensorIndices;
  Tensor sliced = applySlicing(self, indices, tensorIndices);
  if (tensorIndices.empty()) {
    if (sliced.is_same(self)) {
      // ensure we return a shallow copy for things like x[...]
      sliced = sliced.alias();
    }
    return sliced;
  }

  // indexing by tensors ("advanced" indexing)
  return dispatch_index(sliced, tensorIndices);
}

// This mirrors `THPVariable_setitem` in torch/csrc/autograd/python_variable_indexing.cpp
// for "the assigned value is a Tensor" case
void set_item(Tensor& self, ArrayRef<TensorIndex> indices, const Tensor& value) {
  OptionalDeviceGuard device_guard(device_of(self));

  // handle simple types: integers, slices, ellipsis, bool
  if (indices.size() == 1) {
    const TensorIndex& index = indices[0];
    if (!index.is_tensor()) {
      return handleSimpleTypesInSingleDimIndexingSet(self, index, value, /*is_tracing=*/false);
    }
  }

  std::vector<Tensor> tensorIndices;
  Tensor sliced = applySlicing(self, indices, tensorIndices);
  if (tensorIndices.empty()) {
    copy_to(sliced, value);
    return;
  }

  IntArrayRef slicedValueSizes = slicePrefix1sSize(value.sizes());
  Tensor valuesSliced;
  if (!value.sizes().equals(slicedValueSizes)) {
    valuesSliced = value.view(slicedValueSizes);
  } else {
    valuesSliced = value;
  }
  dispatch_index_put_(sliced, tensorIndices, valuesSliced);
  return;
}

// This mirrors `THPVariable_setitem` in torch/csrc/autograd/python_variable_indexing.cpp
// for "the assigned value is a Scalar" case
void set_item(Tensor& self, ArrayRef<TensorIndex> indices, Scalar v) {
  OptionalDeviceGuard device_guard(device_of(self));
  Tensor value;

  // TODO: This qint special case looks very suspicious...
  if (isQIntType(self.scalar_type())) {
    value = at::scalar_tensor(v, device(kCPU).dtype(kFloat));
  } else {
    value = at::scalar_tensor(v, self.options());
  }

  return set_item(self, indices, value);
}

} // namespace indexing

Tensor Tensor::index(ArrayRef<at::indexing::TensorIndex> indices) const {
  return at::indexing::get_item(*this, indices);
}
Tensor Tensor::index(std::initializer_list<at::indexing::TensorIndex> indices) const {
  return index(ArrayRef<at::indexing::TensorIndex>(indices));
}

Tensor & Tensor::index_put_(ArrayRef<at::indexing::TensorIndex> indices, Tensor const & rhs) {
  at::indexing::set_item(*this, indices, rhs);
  return *this;
}
Tensor & Tensor::index_put_(ArrayRef<at::indexing::TensorIndex> indices, Tensor && rhs) {
  at::indexing::set_item(*this, indices, rhs);
  return *this;
}
Tensor & Tensor::index_put_(ArrayRef<at::indexing::TensorIndex> indices, Scalar v) {
  at::indexing::set_item(*this, indices, v);
  return *this;
}
Tensor & Tensor::index_put_(std::initializer_list<at::indexing::TensorIndex> indices, Tensor const & rhs) {
  return index_put_(ArrayRef<at::indexing::TensorIndex>(indices), rhs);
}
Tensor & Tensor::index_put_(std::initializer_list<at::indexing::TensorIndex> indices, Tensor && rhs) {
  return index_put_(ArrayRef<at::indexing::TensorIndex>(indices), rhs);
}
Tensor & Tensor::index_put_(std::initializer_list<at::indexing::TensorIndex> indices, Scalar v) {
  return index_put_(ArrayRef<at::indexing::TensorIndex>(indices), v);
}

} // namespace at
