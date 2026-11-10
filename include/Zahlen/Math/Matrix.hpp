#pragma once

#include <Zahlen/Math/ValueTypes.hpp> // For ZHLN::Float3, ZHLN::Float4
#include <array>
#include <concepts>
#include <cstddef>
#include <mdspan>
#include <type_traits>

namespace ZHLN {

template <typename T, size_t Rows, size_t Cols, size_t Alignment = (Rows == 4 && sizeof(T) == 4 ? 16 : alignof(T))>
struct alignas(Alignment) Matrix {
    static constexpr size_t kRows     = Rows;
    static constexpr size_t kCols     = Cols;
    static constexpr size_t kElements = Rows * Cols;

    std::array<T, kElements> elements {};

    constexpr Matrix() noexcept = default;

    template <typename... Args>
        requires(sizeof...(Args) == kElements && (std::convertible_to<Args, T> && ...))
    constexpr explicit Matrix(Args... args) noexcept: elements {static_cast<T>(args)...} {
    }

    [[nodiscard]] constexpr auto operator[](size_t r, size_t c) noexcept -> T& {
        return elements[c * Rows + r];
    }

    [[nodiscard]] constexpr auto operator[](size_t r, size_t c) const noexcept -> const T& {
        return elements[c * Rows + r];
    }

    [[nodiscard]] static constexpr auto Identity() noexcept -> Matrix {
        Matrix           m {};
        constexpr size_t diag = (Rows < Cols) ? Rows : Cols;
        for (size_t i = 0; i < diag; ++i) {
            m[i, i] = T {1};
        }
        return m;
    }

    [[nodiscard]] static constexpr auto Zero() noexcept -> Matrix {
        return Matrix {};
    }

    [[nodiscard]] static constexpr auto FromCols(Float3 c0, Float3 c1, Float3 c2) noexcept -> Matrix<T, 3, 3, Alignment>
        requires(Rows == 3 && Cols == 3)
    {
        return Matrix<T, 3, 3, Alignment> {c0.x, c0.y, c0.z, c1.x, c1.y, c1.z, c2.x, c2.y, c2.z};
    }

    [[nodiscard]] static constexpr auto FromCols(Float4 c0, Float4 c1, Float4 c2, Float4 c3) noexcept -> Matrix<T, 4, 4, Alignment>
        requires(Rows == 4 && Cols == 4)
    {
        return Matrix<T, 4, 4, Alignment> {c0.x, c0.y, c0.z, c0.w, c1.x, c1.y, c1.z, c1.w, c2.x, c2.y, c2.z, c2.w, c3.x, c3.y, c3.z, c3.w};
    }

    [[nodiscard]] constexpr auto Column(size_t c) const noexcept -> Float3
        requires(Rows == 3)
    {
        return Float3 {(*this)[0, c], (*this)[1, c], (*this)[2, c]};
    }
    [[nodiscard]] constexpr auto Row(size_t r) const noexcept -> Float3
        requires(Cols == 3)
    {
        return Float3 {(*this)[r, 0], (*this)[r, 1], (*this)[r, 2]};
    }

    [[nodiscard]] constexpr auto Column(size_t c) const noexcept -> Float4
        requires(Rows == 4)
    {
        return Float4 {(*this)[0, c], (*this)[1, c], (*this)[2, c], (*this)[3, c]};
    }
    [[nodiscard]] constexpr auto Row(size_t r) const noexcept -> Float4
        requires(Cols == 4)
    {
        return Float4 {(*this)[r, 0], (*this)[r, 1], (*this)[r, 2], (*this)[r, 3]};
    }

    [[nodiscard]] constexpr auto Array() noexcept -> decltype(auto) {
        if constexpr (sizeof(std::array<T, kElements>) <= 64) {
            return elements; // No parens = returns by value (copy)
        } else {
            return (elements); // Extra parens = returns by const reference (&)
        }
    }

    [[nodiscard]] constexpr auto Array() const noexcept -> decltype(auto) {
        if constexpr (sizeof(std::array<T, kElements>) <= 64) {
            return elements;
        } else {
            return (elements);
        }
    }

    [[nodiscard]] constexpr auto data() noexcept -> T* {
        return elements.data();
    }
    [[nodiscard]] constexpr auto data() const noexcept -> const T* {
        return elements.data();
    }

    [[nodiscard]] constexpr auto begin() noexcept -> T* {
        return elements.data();
    }
    [[nodiscard]] constexpr auto begin() const noexcept -> const T* {
        return elements.data();
    }
    [[nodiscard]] constexpr auto end() noexcept -> T* {
        return elements.data() + kElements;
    }
    [[nodiscard]] constexpr auto end() const noexcept -> const T* {
        return elements.data() + kElements;
    }

    [[nodiscard]] constexpr auto MDSpan() noexcept {
        return std::mdspan<T, std::extents<size_t, Rows, Cols>, std::layout_left>(elements.data());
    }

    constexpr auto operator==(const Matrix&) const noexcept -> bool = default;
};

using Mat3   = Matrix<float, 3, 3>;
using Mat4x4 = Matrix<float, 4, 4, 16>;

static_assert(std::is_standard_layout_v<Mat4x4>);
static_assert(std::is_trivially_copyable_v<Mat4x4>);
static_assert(sizeof(Mat4x4) == 64);
static_assert(alignof(Mat4x4) == 16);

} // namespace ZHLN
