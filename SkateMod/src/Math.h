#pragma once

// Small rigid-transform helpers. Matrices are 4x4 column-major (as skate_engine.dll returns
// them), acting on column vectors: p' = M * p. Translation lives in m[12..14].

#include "Common.h"
#include <cmath>

struct Vec3
{
    float X = 0, Y = 0, Z = 0;

    Vec3 operator+(const Vec3& O) const { return { X + O.X, Y + O.Y, Z + O.Z }; }
    Vec3 operator-(const Vec3& O) const { return { X - O.X, Y - O.Y, Z - O.Z }; }
    Vec3 operator*(float S) const { return { X * S, Y * S, Z * S }; }
    float Dot(const Vec3& O) const { return X * O.X + Y * O.Y + Z * O.Z; }
    Vec3 Cross(const Vec3& O) const { return { Y * O.Z - Z * O.Y, Z * O.X - X * O.Z, X * O.Y - Y * O.X }; }
    float Length() const { return std::sqrt(Dot(*this)); }
    Vec3 Normal() const { const float L = Length(); return L > 1e-6f ? *this * (1.f / L) : Vec3{}; }
};

struct Mat4
{
    float M[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

    static Mat4 FromArray(const float* A) { Mat4 R; for (int i = 0; i < 16; i++) R.M[i] = A[i]; return R; }

    Vec3 Column(int C) const { return { M[C * 4], M[C * 4 + 1], M[C * 4 + 2] }; }
    void SetColumn(int C, const Vec3& V) { M[C * 4] = V.X; M[C * 4 + 1] = V.Y; M[C * 4 + 2] = V.Z; }
    Vec3 Translation() const { return Column(3); }
    void SetTranslation(const Vec3& V) { SetColumn(3, V); }

    Mat4 operator*(const Mat4& B) const
    {
        Mat4 R;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
            {
                float S = 0;
                for (int k = 0; k < 4; k++) S += M[k * 4 + r] * B.M[c * 4 + k];
                R.M[c * 4 + r] = S;
            }
        return R;
    }

    Vec3 TransformVector(const Vec3& V) const
    {
        return { M[0] * V.X + M[4] * V.Y + M[8] * V.Z, M[1] * V.X + M[5] * V.Y + M[9] * V.Z, M[2] * V.X + M[6] * V.Y + M[10] * V.Z };
    }
    Vec3 TransformPoint(const Vec3& P) const { return TransformVector(P) + Translation(); }

    // Inverse of a rotation + translation (no scale).
    Mat4 RigidInverse() const
    {
        Mat4 R;
        for (int c = 0; c < 3; c++)
            for (int r = 0; r < 3; r++)
                R.M[c * 4 + r] = M[r * 4 + c];
        R.M[3] = R.M[7] = R.M[11] = 0;
        const Vec3 T = R.TransformVector(Translation());
        R.SetTranslation(T * -1.f);
        R.M[15] = 1;
        return R;
    }

    // Rotation part only (orthonormalised: removes drift and any scale).
    Mat4 Rotation() const
    {
        Mat4 R;
        const Vec3 X = Column(0).Normal();
        const Vec3 Z = X.Cross(Column(1)).Normal();
        const Vec3 Y = Z.Cross(X);
        R.SetColumn(0, X); R.SetColumn(1, Y); R.SetColumn(2, Z);
        return R;
    }

    static Mat4 FromBasis(const Vec3& X, const Vec3& Y, const Vec3& Z)
    {
        Mat4 R; R.SetColumn(0, X); R.SetColumn(1, Y); R.SetColumn(2, Z); return R;
    }

    // Rotation taking direction From onto direction To (both normalised).
    static Mat4 Arc(const Vec3& From, const Vec3& To)
    {
        const Vec3 Axis = From.Cross(To);
        const float C = From.Dot(To);
        if (C < -0.9999f)
        {
            // 180 degrees: any axis perpendicular to From.
            Vec3 P = std::fabs(From.X) < 0.9f ? Vec3{ 1, 0, 0 } : Vec3{ 0, 1, 0 };
            P = From.Cross(P).Normal();
            Mat4 R;
            for (int c = 0; c < 3; c++)
                for (int r = 0; r < 3; r++)
                {
                    const float Pc = c == 0 ? P.X : c == 1 ? P.Y : P.Z, Pr = r == 0 ? P.X : r == 1 ? P.Y : P.Z;
                    R.M[c * 4 + r] = 2 * Pr * Pc - (r == c ? 1.f : 0.f);
                }
            return R;
        }
        const float K = 1.f / (1.f + C);
        Mat4 R;
        R.M[0] = Axis.X * Axis.X * K + C;      R.M[4] = Axis.X * Axis.Y * K - Axis.Z; R.M[8] = Axis.X * Axis.Z * K + Axis.Y;
        R.M[1] = Axis.Y * Axis.X * K + Axis.Z; R.M[5] = Axis.Y * Axis.Y * K + C;      R.M[9] = Axis.Y * Axis.Z * K - Axis.X;
        R.M[2] = Axis.Z * Axis.X * K - Axis.Y; R.M[6] = Axis.Z * Axis.Y * K + Axis.X; R.M[10] = Axis.Z * Axis.Z * K + C;
        return R;
    }
};

inline SDK::FQuat QuatFromMatrix(const Mat4& In)
{
    const Mat4 R = In.Rotation();
    auto m = [&](int Row, int Col) { return double(R.M[Col * 4 + Row]); };
    SDK::FQuat Q{};
    const double Trace = m(0, 0) + m(1, 1) + m(2, 2);
    if (Trace > 0)
    {
        const double S = std::sqrt(Trace + 1.0) * 2;
        Q.W = 0.25 * S; Q.X = (m(2, 1) - m(1, 2)) / S; Q.Y = (m(0, 2) - m(2, 0)) / S; Q.Z = (m(1, 0) - m(0, 1)) / S;
    }
    else if (m(0, 0) > m(1, 1) && m(0, 0) > m(2, 2))
    {
        const double S = std::sqrt(1.0 + m(0, 0) - m(1, 1) - m(2, 2)) * 2;
        Q.W = (m(2, 1) - m(1, 2)) / S; Q.X = 0.25 * S; Q.Y = (m(0, 1) + m(1, 0)) / S; Q.Z = (m(0, 2) + m(2, 0)) / S;
    }
    else if (m(1, 1) > m(2, 2))
    {
        const double S = std::sqrt(1.0 + m(1, 1) - m(0, 0) - m(2, 2)) * 2;
        Q.W = (m(0, 2) - m(2, 0)) / S; Q.X = (m(0, 1) + m(1, 0)) / S; Q.Y = 0.25 * S; Q.Z = (m(1, 2) + m(2, 1)) / S;
    }
    else
    {
        const double S = std::sqrt(1.0 + m(2, 2) - m(0, 0) - m(1, 1)) * 2;
        Q.W = (m(1, 0) - m(0, 1)) / S; Q.X = (m(0, 2) + m(2, 0)) / S; Q.Y = (m(1, 2) + m(2, 1)) / S; Q.Z = 0.25 * S;
    }
    return Q;
}

inline Mat4 MatrixFromQuat(const SDK::FQuat& Q)
{
    const float X = float(Q.X), Y = float(Q.Y), Z = float(Q.Z), W = float(Q.W);
    Mat4 R;
    R.M[0] = 1 - 2 * (Y * Y + Z * Z); R.M[4] = 2 * (X * Y - Z * W);     R.M[8] = 2 * (X * Z + Y * W);
    R.M[1] = 2 * (X * Y + Z * W);     R.M[5] = 1 - 2 * (X * X + Z * Z); R.M[9] = 2 * (Y * Z - X * W);
    R.M[2] = 2 * (X * Z - Y * W);     R.M[6] = 2 * (Y * Z + X * W);     R.M[10] = 1 - 2 * (X * X + Y * Y);
    return R;
}

inline SDK::FTransform ToTransform(const Mat4& M)
{
    SDK::FTransform T{};
    T.Rotation = QuatFromMatrix(M);
    const Vec3 P = M.Translation();
    T.Translation = SDK::FVector{ P.X, P.Y, P.Z };
    T.Scale3D = SDK::FVector{ 1, 1, 1 };
    return T;
}

inline Mat4 FromTransform(const SDK::FTransform& T)
{
    Mat4 M = MatrixFromQuat(T.Rotation);
    M.SetTranslation({ float(T.Translation.X), float(T.Translation.Y), float(T.Translation.Z) });
    return M;
}
