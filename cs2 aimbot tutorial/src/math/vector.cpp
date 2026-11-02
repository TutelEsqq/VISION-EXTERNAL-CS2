#include "vector.hpp"

// captured once at startup; a resolution change while the cheat is running will skew w2s
static const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
static const int screenHeight = GetSystemMetrics(SM_CYSCREEN);

Vector Vector::operator-(const Vector& other) const noexcept
{
    return Vector{ x - other.x, y - other.y, z - other.z };
}

Vector Vector::operator+(const Vector& other) const noexcept
{
    return Vector{ x + other.x, y + other.y, z + other.z };
}

Vector Vector::operator/(float factor) const noexcept
{
    return Vector{ x / factor, y / factor, z / factor };
}

Vector Vector::operator*(float factor) const noexcept
{
    return Vector{ x * factor, y * factor, z * factor };
}

// w2s, alot of explanations already exist
bool Vector::world_to_screen(const view_matrix_t& vm, const Vector& in, Vector& out)
{
    return world_to_screen(vm, in, out, screenWidth, screenHeight);
}

// explicit surface size - the overlay draws into its own client rect, which is not
// the desktop size when the game runs windowed or borderless
bool Vector::world_to_screen(const view_matrix_t& vm, const Vector& in, Vector& out, int width, int height)
{
    out.x = vm[0][0] * in.x + vm[0][1] * in.y + vm[0][2] * in.z + vm[0][3];
    out.y = vm[1][0] * in.x + vm[1][1] * in.y + vm[1][2] * in.z + vm[1][3];

    float w = vm[3][0] * in.x + vm[3][1] * in.y + vm[3][2] * in.z + vm[3][3];

    if (w < 0.01f)
        return false;

    float inverseWidth = 1.f / w;

    out.x *= inverseWidth;
    out.y *= inverseWidth;

    float x = static_cast<float>(width) * 0.5f;
    float y = static_cast<float>(height) * 0.5f;

    x += 0.5f * out.x * width + 0.5f;
    y -= 0.5f * out.y * height + 0.5f;

    out.x = x;
    out.y = y;
    out.z = 0.f;

    return true;
}

bool Vector::IsZero() const
{
    return x == 0.f && y == 0.f && z == 0.f;
}

bool Vector::IsFinite() const
{
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
