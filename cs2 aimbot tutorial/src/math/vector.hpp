#pragma once
#include <cmath>
#include <Windows.h>

struct view_matrix_t
{
	float* operator[](int index)
	{
		return matrix[index];
	}

	const float* operator[](int index) const
	{
		return matrix[index];
	}

	float matrix[4][4];
};

class Vector
{
public:
	constexpr Vector(
		const float x = 0.f,
		const float y = 0.f,
		const float z = 0.f) noexcept :
		x(x), y(y), z(z) { }

	// by value, not by reference: the old signatures returned a reference to a temporary (dangling)
	Vector operator-(const Vector& other) const noexcept;
	Vector operator+(const Vector& other) const noexcept;
	Vector operator/(float factor) const noexcept;
	Vector operator*(float factor) const noexcept;

	// 3d -> 2d, explanations already exist.
	static bool world_to_screen(const view_matrix_t& view_matrix, const Vector& in, Vector& out);
	static bool world_to_screen(const view_matrix_t& view_matrix, const Vector& in, Vector& out, int width, int height);

	bool IsZero() const;
	bool IsFinite() const;

	float x, y, z;
};
