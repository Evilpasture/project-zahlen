// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <mdspan>
#include <numbers>
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

    [[nodiscard]] static constexpr auto FromCols(Vec3<T> c0, Vec3<T> c1, Vec3<T> c2) noexcept -> Matrix<T, 3, 3, Alignment>
        requires(Rows == 3 && Cols == 3)
    {
        return Matrix<T, 3, 3, Alignment> {c0.x, c0.y, c0.z, c1.x, c1.y, c1.z, c2.x, c2.y, c2.z};
    }

    [[nodiscard]] static constexpr auto FromCols(Vec4<T> c0, Vec4<T> c1, Vec4<T> c2, Vec4<T> c3) noexcept -> Matrix<T, 4, 4, Alignment>
        requires(Rows == 4 && Cols == 4)
    {
        return Matrix<T, 4, 4, Alignment> {c0.x, c0.y, c0.z, c0.w, c1.x, c1.y, c1.z, c1.w, c2.x, c2.y, c2.z, c2.w, c3.x, c3.y, c3.z, c3.w};
    }

    [[nodiscard]] constexpr auto Column(size_t c) const noexcept -> Vec3<T>
        requires(Rows == 3)
    {
        return Vec3<T> {(*this)[0, c], (*this)[1, c], (*this)[2, c]};
    }

    [[nodiscard]] constexpr auto Row(size_t r) const noexcept -> Vec3<T>
        requires(Cols == 3)
    {
        return Vec3<T> {(*this)[r, 0], (*this)[r, 1], (*this)[r, 2]};
    }

    [[nodiscard]] constexpr auto Column(size_t c) const noexcept -> Vec4<T>
        requires(Rows == 4)
    {
        return Vec4<T> {(*this)[0, c], (*this)[1, c], (*this)[2, c], (*this)[3, c]};
    }

    [[nodiscard]] constexpr auto Row(size_t r) const noexcept -> Vec4<T>
        requires(Cols == 4)
    {
        return Vec4<T> {(*this)[r, 0], (*this)[r, 1], (*this)[r, 2], (*this)[r, 3]};
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

    constexpr auto operator+=(const Matrix& rhs) noexcept -> Matrix& {
        for (size_t i = 0; i < kElements; ++i)
            elements[i] += rhs.elements[i];
        return *this;
    }

    constexpr auto operator-=(const Matrix& rhs) noexcept -> Matrix& {
        for (size_t i = 0; i < kElements; ++i)
            elements[i] -= rhs.elements[i];
        return *this;
    }

    constexpr auto operator*=(T scalar) noexcept -> Matrix& {
        for (size_t i = 0; i < kElements; ++i)
            elements[i] *= scalar;
        return *this;
    }

    constexpr auto operator/=(T scalar) noexcept -> Matrix& {
        for (size_t i = 0; i < kElements; ++i)
            elements[i] /= scalar;
        return *this;
    }
};

template <typename T, size_t R, size_t C, size_t A>
[[nodiscard]] constexpr auto operator+(Matrix<T, R, C, A> lhs, const Matrix<T, R, C, A>& rhs) noexcept -> Matrix<T, R, C, A> {
    lhs += rhs;
    return lhs;
}

template <typename T, size_t R, size_t C, size_t A>
[[nodiscard]] constexpr auto operator-(Matrix<T, R, C, A> lhs, const Matrix<T, R, C, A>& rhs) noexcept -> Matrix<T, R, C, A> {
    lhs -= rhs;
    return lhs;
}

template <typename T, size_t R, size_t C, size_t A>
[[nodiscard]] constexpr auto operator*(Matrix<T, R, C, A> m, T scalar) noexcept -> Matrix<T, R, C, A> {
    m *= scalar;
    return m;
}

template <typename T, size_t R, size_t C, size_t A>
[[nodiscard]] constexpr auto operator*(T scalar, Matrix<T, R, C, A> m) noexcept -> Matrix<T, R, C, A> {
    m *= scalar;
    return m;
}

template <typename T, size_t R, size_t C, size_t A>
[[nodiscard]] constexpr auto operator/(Matrix<T, R, C, A> m, T scalar) noexcept -> Matrix<T, R, C, A> {
    m /= scalar;
    return m;
}

template <typename T, size_t R, size_t K, size_t C, size_t A1, size_t A2>
[[nodiscard]] constexpr auto operator*(const Matrix<T, R, K, A1>& lhs, const Matrix<T, K, C, A2>& rhs) noexcept -> Matrix<T, R, C, A1> {
    Matrix<T, R, C, A1> result {};
    for (size_t c = 0; c < C; ++c) {
        for (size_t k = 0; k < K; ++k) {
            const T rhs_val = rhs[k, c];
            for (size_t r = 0; r < R; ++r) {
                result[r, c] += lhs[r, k] * rhs_val;
            }
        }
    }
    return result;
}

template <typename T, size_t A>
[[nodiscard]] constexpr auto operator*(const Matrix<T, 3, 3, A>& m, Vec3<T> v) noexcept -> Vec3<T> {
    return {m[0, 0] * v.x + m[0, 1] * v.y + m[0, 2] * v.z, m[1, 0] * v.x + m[1, 1] * v.y + m[1, 2] * v.z, m[2, 0] * v.x + m[2, 1] * v.y + m[2, 2] * v.z};
}

template <typename T, size_t A>
[[nodiscard]] constexpr auto operator*(const Matrix<T, 4, 4, A>& m, Vec4<T> v) noexcept -> Vec4<T> {
    return {
        m[0, 0] * v.x + m[0, 1] * v.y + m[0, 2] * v.z + m[0, 3] * v.w, m[1, 0] * v.x + m[1, 1] * v.y + m[1, 2] * v.z + m[1, 3] * v.w,
        m[2, 0] * v.x + m[2, 1] * v.y + m[2, 2] * v.z + m[2, 3] * v.w, m[3, 0] * v.x + m[3, 1] * v.y + m[3, 2] * v.z + m[3, 3] * v.w
    };
}

template <typename T, size_t A>
[[nodiscard]] constexpr auto TransformPoint(const Matrix<T, 4, 4, A>& m, Vec3<T> p) noexcept -> Vec3<T> {
    const Vec4<T> res = m * Vec4<T> {p.x, p.y, p.z, T {1}};
    if (res.w != T {0} && res.w != T {1}) {
        return {res.x / res.w, res.y / res.w, res.z / res.w};
    }
    return {res.x, res.y, res.z};
}

template <typename T, size_t A>
[[nodiscard]] constexpr auto TransformVector(const Matrix<T, 4, 4, A>& m, Vec3<T> d) noexcept -> Vec3<T> {
    const Vec4<T> res = m * Vec4<T> {d.x, d.y, d.z, T {0}};
    return {res.x, res.y, res.z};
}

template <typename T, size_t Rows, size_t Cols, size_t A>
[[nodiscard]] constexpr auto Transpose(const Matrix<T, Rows, Cols, A>& m) noexcept -> Matrix<T, Cols, Rows, A> {
    Matrix<T, Cols, Rows, A> res {};
    for (size_t r = 0; r < Rows; ++r) {
        for (size_t c = 0; c < Cols; ++c) {
            res[c, r] = m[r, c];
        }
    }
    return res;
}

template <typename T, size_t A>
[[nodiscard]] constexpr auto Determinant(const Matrix<T, 3, 3, A>& m) noexcept -> T {
    return m[0, 0] * (m[1, 1] * m[2, 2] - m[2, 1] * m[1, 2]) - m[0, 1] * (m[1, 0] * m[2, 2] - m[2, 0] * m[1, 2]) +
           m[0, 2] * (m[1, 0] * m[2, 1] - m[2, 0] * m[1, 1]);
}

template <typename T, size_t A>
[[nodiscard]] constexpr auto Inverse(const Matrix<T, 3, 3, A>& m) noexcept -> Matrix<T, 3, 3, A> {
    const T det = Determinant(m);
    if (std::abs(det) <= T {1e-8})
        return Matrix<T, 3, 3, A>::Identity();
    const T invDet = T {1} / det;

    Matrix<T, 3, 3, A> inv {};
    inv[0, 0] = (m[1, 1] * m[2, 2] - m[2, 1] * m[1, 2]) * invDet;
    inv[0, 1] = -(m[0, 1] * m[2, 2] - m[2, 1] * m[0, 2]) * invDet;
    inv[0, 2] = (m[0, 1] * m[1, 2] - m[1, 1] * m[0, 2]) * invDet;

    inv[1, 0] = -(m[1, 0] * m[2, 2] - m[2, 0] * m[1, 2]) * invDet;
    inv[1, 1] = (m[0, 0] * m[2, 2] - m[2, 0] * m[0, 2]) * invDet;
    inv[1, 2] = -(m[0, 0] * m[1, 2] - m[1, 0] * m[0, 2]) * invDet;

    inv[2, 0] = (m[1, 0] * m[2, 1] - m[2, 0] * m[1, 1]) * invDet;
    inv[2, 1] = -(m[0, 0] * m[2, 1] - m[2, 0] * m[0, 1]) * invDet;
    inv[2, 2] = (m[0, 0] * m[1, 1] - m[1, 0] * m[0, 1]) * invDet;
    return inv;
}

template <typename T, size_t A>
[[nodiscard]] constexpr auto Determinant(const Matrix<T, 4, 4, A>& m) noexcept -> T {
    const T s0 = m[0, 0] * m[1, 1] - m[1, 0] * m[0, 1];
    const T s1 = m[0, 0] * m[1, 2] - m[1, 0] * m[0, 2];
    const T s2 = m[0, 0] * m[1, 3] - m[1, 0] * m[0, 3];
    const T s3 = m[0, 1] * m[1, 2] - m[1, 1] * m[0, 2];
    const T s4 = m[0, 1] * m[1, 3] - m[1, 1] * m[0, 3];
    const T s5 = m[0, 2] * m[1, 3] - m[1, 2] * m[0, 3];

    const T c5 = m[2, 2] * m[3, 3] - m[3, 2] * m[2, 3];
    const T c4 = m[2, 1] * m[3, 3] - m[3, 1] * m[2, 3];
    const T c3 = m[2, 1] * m[3, 2] - m[3, 1] * m[2, 2];
    const T c2 = m[2, 0] * m[3, 3] - m[3, 0] * m[2, 3];
    const T c1 = m[2, 0] * m[3, 2] - m[3, 0] * m[2, 2];
    const T c0 = m[2, 0] * m[3, 1] - m[3, 0] * m[2, 1];

    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

template <typename T, size_t A>
[[nodiscard]] constexpr auto Inverse(const Matrix<T, 4, 4, A>& m) noexcept -> Matrix<T, 4, 4, A> {
    const T s0 = m[0, 0] * m[1, 1] - m[1, 0] * m[0, 1];
    const T s1 = m[0, 0] * m[1, 2] - m[1, 0] * m[0, 2];
    const T s2 = m[0, 0] * m[1, 3] - m[1, 0] * m[0, 3];
    const T s3 = m[0, 1] * m[1, 2] - m[1, 1] * m[0, 2];
    const T s4 = m[0, 1] * m[1, 3] - m[1, 1] * m[0, 3];
    const T s5 = m[0, 2] * m[1, 3] - m[1, 2] * m[0, 3];

    const T c5 = m[2, 2] * m[3, 3] - m[3, 2] * m[2, 3];
    const T c4 = m[2, 1] * m[3, 3] - m[3, 1] * m[2, 3];
    const T c3 = m[2, 1] * m[3, 2] - m[3, 1] * m[2, 2];
    const T c2 = m[2, 0] * m[3, 3] - m[3, 0] * m[2, 3];
    const T c1 = m[2, 0] * m[3, 2] - m[3, 0] * m[2, 2];
    const T c0 = m[2, 0] * m[3, 1] - m[3, 0] * m[2, 1];

    const T det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (std::abs(det) <= T {1e-8})
        return Matrix<T, 4, 4, A>::Identity();
    const T invDet = T {1} / det;

    Matrix<T, 4, 4, A> inv {};
    inv[0, 0] = (m[1, 1] * c5 - m[1, 2] * c4 + m[1, 3] * c3) * invDet;
    inv[0, 1] = -(m[0, 1] * c5 - m[0, 2] * c4 + m[0, 3] * c3) * invDet;
    inv[0, 2] = (m[3, 1] * s5 - m[3, 2] * s4 + m[3, 3] * s3) * invDet;
    inv[0, 3] = -(m[2, 1] * s5 - m[2, 2] * s4 + m[2, 3] * s3) * invDet;

    inv[1, 0] = -(m[1, 0] * c5 - m[1, 2] * c2 + m[1, 3] * c1) * invDet;
    inv[1, 1] = (m[0, 0] * c5 - m[0, 2] * c2 + m[0, 3] * c1) * invDet;
    inv[1, 2] = -(m[3, 0] * s5 - m[3, 2] * s2 + m[3, 3] * s1) * invDet;
    inv[1, 3] = (m[2, 0] * s5 - m[2, 2] * s2 + m[2, 3] * s1) * invDet;

    inv[2, 0] = (m[1, 0] * c4 - m[1, 1] * c2 + m[1, 3] * c0) * invDet;
    inv[2, 1] = -(m[0, 0] * c4 - m[0, 1] * c2 + m[0, 3] * c0) * invDet;
    inv[2, 2] = (m[3, 0] * s4 - m[3, 1] * s2 + m[3, 3] * s0) * invDet;
    inv[2, 3] = -(m[2, 0] * s4 - m[2, 1] * s2 + m[2, 3] * s0) * invDet;

    inv[3, 0] = -(m[1, 0] * c3 - m[1, 1] * c1 + m[1, 2] * c0) * invDet;
    inv[3, 1] = (m[0, 0] * c3 - m[0, 1] * c1 + m[0, 2] * c0) * invDet;
    inv[3, 2] = -(m[3, 0] * s3 - m[3, 1] * s1 + m[3, 2] * s0) * invDet;
    inv[3, 3] = (m[2, 0] * s3 - m[2, 1] * s1 + m[2, 2] * s0) * invDet;

    return inv;
}

template <typename T>
[[nodiscard]] constexpr auto CreateTranslation(Vec3<T> t) noexcept -> Matrix<T, 4, 4> {
    auto m  = Matrix<T, 4, 4>::Identity();
    m[0, 3] = t.x;
    m[1, 3] = t.y;
    m[2, 3] = t.z;
    return m;
}

template <typename T>
[[nodiscard]] constexpr auto CreateScale(Vec3<T> s) noexcept -> Matrix<T, 4, 4> {
    auto m  = Matrix<T, 4, 4>::Zero();
    m[0, 0] = s.x;
    m[1, 1] = s.y;
    m[2, 2] = s.z;
    m[3, 3] = T {1};
    return m;
}

template <typename T>
[[nodiscard]] auto CreateRotationX(T angleRad) noexcept -> Matrix<T, 4, 4> {
    auto    m = Matrix<T, 4, 4>::Identity();
    const T c = std::cos(angleRad);
    const T s = std::sin(angleRad);
    m[1, 1]   = c;
    m[1, 2]   = -s;
    m[2, 1]   = s;
    m[2, 2]   = c;
    return m;
}

template <typename T>
[[nodiscard]] auto CreateRotationY(T angleRad) noexcept -> Matrix<T, 4, 4> {
    auto    m = Matrix<T, 4, 4>::Identity();
    const T c = std::cos(angleRad);
    const T s = std::sin(angleRad);
    m[0, 0]   = c;
    m[0, 2]   = s;
    m[2, 0]   = -s;
    m[2, 2]   = c;
    return m;
}

template <typename T>
[[nodiscard]] auto CreateRotationZ(T angleRad) noexcept -> Matrix<T, 4, 4> {
    auto    m = Matrix<T, 4, 4>::Identity();
    const T c = std::cos(angleRad);
    const T s = std::sin(angleRad);
    m[0, 0]   = c;
    m[0, 1]   = -s;
    m[1, 0]   = s;
    m[1, 1]   = c;
    return m;
}

template <typename T>
[[nodiscard]] auto CreateLookAt(Vec3<T> eye, Vec3<T> target, Vec3<T> up) noexcept -> Matrix<T, 4, 4> {
    const Vec3<T> z = (eye - target).Normalized();
    const Vec3<T> x = Cross(up, z).Normalized();
    const Vec3<T> y = Cross(z, x);

    Matrix<T, 4, 4> m {};
    m[0, 0] = x.x;
    m[0, 1] = x.y;
    m[0, 2] = x.z;
    m[0, 3] = -Dot(x, eye);
    m[1, 0] = y.x;
    m[1, 1] = y.y;
    m[1, 2] = y.z;
    m[1, 3] = -Dot(y, eye);
    m[2, 0] = z.x;
    m[2, 1] = z.y;
    m[2, 2] = z.z;
    m[2, 3] = -Dot(z, eye);
    m[3, 3] = T {1};
    return m;
}

template <typename T>
[[nodiscard]] auto CreatePerspective(T fovRadians, T aspect, T nearZ, T farZ) noexcept -> Matrix<T, 4, 4> {
    const T         f = T {1} / std::tan(fovRadians * T {0.5});
    Matrix<T, 4, 4> m {};
    m[0, 0] = f / aspect;
    m[1, 1] = -f; // Vulkan Y-down flip
    m[2, 2] = farZ / (nearZ - farZ);
    m[2, 3] = (nearZ * farZ) / (nearZ - farZ);
    m[3, 2] = T {-1};
    return m;
}

template <typename T>
[[nodiscard]] constexpr auto CreateOrtho(T left, T right, T bottom, T top, T nearZ, T farZ) noexcept -> Matrix<T, 4, 4> {
    const T r_l = right - left;
    const T t_b = top - bottom;
    const T f_n = farZ - nearZ;

    Matrix<T, 4, 4> m {};
    m[0, 0] = T {2} / r_l;
    m[1, 1] = -T {2} / t_b; // Vulkan Y-down flip
    m[2, 2] = -T {1} / f_n;
    m[0, 3] = -(right + left) / r_l;
    m[1, 3] = (top + bottom) / t_b;
    m[2, 3] = -nearZ / f_n;
    m[3, 3] = T {1};
    return m;
}

using Mat3    = Matrix<float, 3, 3>;
using Mat4x4  = Matrix<float, 4, 4, 16>;
using DMat3   = Matrix<double, 3, 3>;
using DMat4x4 = Matrix<double, 4, 4, 32>;

static_assert(std::is_standard_layout_v<Mat4x4>);
static_assert(std::is_trivially_copyable_v<Mat4x4>);
static_assert(sizeof(Mat4x4) == 64);
static_assert(alignof(Mat4x4) == 16);

} // namespace ZHLN
