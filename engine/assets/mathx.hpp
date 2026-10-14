// Minimal vector/quaternion math for asset posing. Standard conventions:
// quaternions rotate column vectors as q * v * q^-1 (the asset readers convert from the file convention).
#pragma once

#include <cmath>

namespace ron {

struct Vec3 {
	float x = 0, y = 0, z = 0;
};

struct Quat {
	float x = 0, y = 0, z = 0, w = 1;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

inline Quat operator*(Quat a, Quat b) {
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

inline Vec3 rotate(Quat q, Vec3 v) {
	const float tx = 2 * (q.y * v.z - q.z * v.y);
	const float ty = 2 * (q.z * v.x - q.x * v.z);
	const float tz = 2 * (q.x * v.y - q.y * v.x);
	return {
		v.x + q.w * tx + (q.y * tz - q.z * ty),
		v.y + q.w * ty + (q.z * tx - q.x * tz),
		v.z + q.w * tz + (q.x * ty - q.y * tx),
	};
}

inline Quat slerp(Quat a, Quat b, float t) {
	float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	const float s = d < 0 ? -1.0f : 1.0f;
	d *= s;
	float wa, wb;
	if (d > 0.9995f) {
		wa = 1 - t;
		wb = s * t;
	} else {
		const float th = std::acos(d), sn = std::sin(th);
		wa = std::sin((1 - t) * th) / sn;
		wb = s * std::sin(t * th) / sn;
	}
	Quat r{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
	const float len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
	return {r.x / len, r.y / len, r.z / len, r.w / len};
}

// Rigid transform: rotate, then translate. (A * B) applies B first.
struct Xform {
	Quat q;
	Vec3 t;
};

inline Xform operator*(const Xform& a, const Xform& b) { return {a.q * b.q, rotate(a.q, b.t) + a.t}; }
inline Vec3 apply(const Xform& x, Vec3 v) { return rotate(x.q, v) + x.t; }

}  // namespace ron
