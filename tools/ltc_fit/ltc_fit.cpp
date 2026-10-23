// kast_ltc_fit: fits the Linearly Transformed Cosines (Heitz et al. 2016) lookup table area lights shade with, for the
// GGX BRDF of fs_mesh.sc shade(), and writes it as engine/shaders/ltc_table.sh. See README.md next to this file.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <functional>
#include <iterator>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kSize = 16;               // entries per axis: LTC_SIZE in fs_mesh.sc, kLtcSize in renderer.h
constexpr double kMinRoughness = 0.04;  // fs_mesh.sc floors roughness here
constexpr int kFitSamples = 32;         // per axis, per distribution: the fit error's stratified samples
constexpr int kMomentSamples = 256;     // per axis: the BRDF's albedo and average direction
constexpr double kPi = 3.14159265358979323846;

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalize(Vec3 a) { return a * (1.0 / length(a)); }

using Mat3 = std::array<std::array<double, 3>, 3>; // [row][column], acting on column vectors

Vec3 mul(const Mat3& m, Vec3 v)
{
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
}

Mat3 mul(const Mat3& a, const Mat3& b)
{
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                r[i][j] += a[i][k] * b[k][j];
    return r;
}

double determinant(const Mat3& m)
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Mat3 inverse(const Mat3& m)
{
    const double inv = 1.0 / determinant(m);
    Mat3 r;
    r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
    return r;
}

// The specular BRDF of fs_mesh.sc shade() with Fresnel 1, times N.L, around the normal +Z:
// D * G / (4 N.V N.L) * N.L, D = GGX, G = Smith Schlick-GGX with k = (roughness + 1)^2 / 8, alpha = roughness^2.
double ggxD(double nh, double roughness)
{
    const double a2 = std::pow(roughness, 4.0);
    const double denom = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (kPi * denom * denom);
}

double brdfCos(Vec3 v, Vec3 l, double roughness)
{
    if (l.z <= 0.0)
        return 0.0;
    const Vec3 h = normalize(v + l);
    const double nv = std::max(v.z, 1e-4), nl = l.z;
    const double k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    const double g = nv / (nv * (1.0 - k) + k) * (nl / (nl * (1.0 - k) + k));
    return ggxD(std::max(h.z, 0.0), roughness) * g / (4.0 * nv);
}

// GGX normal distribution sampling: h with density D(h) N.H, l = v reflected about h (may fall below the horizon).
Vec3 sampleGgx(Vec3 v, double roughness, double u1, double u2)
{
    const double alpha = roughness * roughness;
    const double phi = 2.0 * kPi * u2;
    const double cosTheta = 1.0 / std::sqrt(1.0 + alpha * alpha * u1 / (1.0 - u1));
    const double sinTheta = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
    const Vec3 h{sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
    return h * (2.0 * dot(v, h)) - v;
}

double ggxPdf(Vec3 v, Vec3 l, double roughness)
{
    const Vec3 h = normalize(v + l);
    const double vh = dot(v, h);
    if (vh <= 0.0 || h.z <= 0.0)
        return 0.0;
    return ggxD(h.z, roughness) * h.z / (4.0 * vh);
}

// A clamped cosine around +Z transformed by m (Heitz et al. 2016): a distribution on the sphere.
struct Ltc {
    Mat3 m{}, inv{};
    double invDet = 0.0;

    explicit Ltc(const Mat3& matrix) : m(matrix), inv(inverse(matrix)), invDet(std::abs(determinant(inv))) {}

    double eval(Vec3 l) const
    {
        const Vec3 o = mul(inv, l);
        const double len = length(o);
        return std::max(o.z / len, 0.0) / kPi * invDet / (len * len * len);
    }
    Vec3 sample(double u1, double u2) const
    {
        const double r = std::sqrt(u1), phi = 2.0 * kPi * u2;
        return normalize(mul(m, Vec3{r * std::cos(phi), r * std::sin(phi), std::sqrt(1.0 - u1)}));
    }
};

// The BRDF lobe's integrals for view v: albedo split by Schlick's Fresnel weight w = (1 - V.H)^5 (F = F0 (1 - w) + w,
// so the Fresnel-weighted albedo is F0 * scale + bias), and its average direction.
struct Moments {
    double scale = 0.0, bias = 0.0;
    Vec3 average{0.0, 0.0, 1.0};
};

Moments moments(Vec3 v, double roughness)
{
    Moments out;
    Vec3 sum;
    for (int i = 0; i < kMomentSamples; ++i)
        for (int j = 0; j < kMomentSamples; ++j) {
            const Vec3 l = sampleGgx(v, roughness, (i + 0.5) / kMomentSamples, (j + 0.5) / kMomentSamples);
            const double pdf = ggxPdf(v, l, roughness);
            if (l.z <= 0.0 || pdf <= 0.0)
                continue;
            const double weight = brdfCos(v, l, roughness) / pdf;
            const double w = std::pow(1.0 - std::clamp(dot(v, normalize(v + l)), 0.0, 1.0), 5.0);
            out.scale += weight * (1.0 - w);
            out.bias += weight * w;
            sum = sum + l * weight;
        }
    const double n = static_cast<double>(kMomentSamples) * kMomentSamples;
    out.scale /= n;
    out.bias /= n;
    sum.y = 0.0; // symmetric about the XZ plane
    out.average = normalize(sum);
    return out;
}

// The fitted LTC's matrix: the average direction's frame (X, Y, Z = average) times a scale and skew (Heitz et al.):
// p = (m00, m11, m02).
Mat3 ltcMatrix(const std::array<double, 3>& p, Vec3 average)
{
    const Vec3 z = average, x = normalize(Vec3{z.z, 0.0, -z.x});
    const Mat3 frame{{{x.x, 0.0, z.x}, {x.y, 1.0, z.y}, {x.z, 0.0, z.z}}};
    const Mat3 shape{{{p[0], 0.0, p[2]}, {0.0, p[1], 0.0}, {0.0, 0.0, 1.0}}};
    return mul(frame, shape);
}

// How far the LTC is from the normalized BRDF lobe: |f - g|^3 over the sum of both sampling pdfs (multiple importance
// sampling), with samples drawn from each.
double fitError(const std::array<double, 3>& p, Vec3 v, double roughness, const Moments& lobe)
{
    if (p[0] <= 1e-5 || p[1] <= 1e-5)
        return 1e30;
    const Ltc ltc(ltcMatrix(p, lobe.average));
    const double norm = lobe.scale + lobe.bias;
    double error = 0.0;
    for (int i = 0; i < kFitSamples; ++i)
        for (int j = 0; j < kFitSamples; ++j) {
            const double u1 = (i + 0.5) / kFitSamples, u2 = (j + 0.5) / kFitSamples;
            for (const Vec3& l : {ltc.sample(u1, u2), sampleGgx(v, roughness, u1, u2)}) {
                const double f = brdfCos(v, l, roughness) / norm;
                const double g = ltc.eval(l);
                const double pdf = g + ggxPdf(v, l, roughness);
                if (pdf > 0.0)
                    error += std::pow(std::abs(f - g), 3.0) / pdf;
            }
        }
    return error / (2.0 * kFitSamples * kFitSamples);
}

// Nelder-Mead downhill simplex; free: the parameters it may move (the rest stay at start's values).
template <typename F>
std::array<double, 3> minimize(F f, std::array<double, 3> start, const std::vector<int>& free)
{
    const size_t n = free.size();
    std::vector<std::array<double, 3>> simplex(n + 1, start);
    for (size_t i = 0; i < n; ++i)
        simplex[i + 1][free[i]] += std::max(0.05, std::abs(start[free[i]]) * 0.1);
    std::vector<double> values;
    for (const auto& point : simplex)
        values.push_back(f(point));
    const auto blend = [&](const std::array<double, 3>& a, const std::array<double, 3>& b, double t) {
        std::array<double, 3> r = a;
        for (int i : free)
            r[i] = a[i] + (b[i] - a[i]) * t;
        return r;
    };
    for (int iteration = 0; iteration < 400; ++iteration) {
        std::vector<size_t> order(n + 1);
        for (size_t i = 0; i <= n; ++i)
            order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return values[a] < values[b]; });
        const size_t best = order.front(), worst = order.back(), second = order[n - 1];
        if (std::abs(values[worst] - values[best]) <= 1e-10 * (std::abs(values[best]) + 1e-20))
            break;
        std::array<double, 3> centroid = start;
        for (int i : free) {
            centroid[i] = 0.0;
            for (size_t k = 0; k <= n; ++k)
                if (k != worst)
                    centroid[i] += simplex[k][i] / static_cast<double>(n);
        }
        const auto reflected = blend(centroid, simplex[worst], -1.0);
        const double fr = f(reflected);
        if (fr < values[best]) {
            const auto expanded = blend(centroid, simplex[worst], -2.0);
            const double fe = f(expanded);
            simplex[worst] = fe < fr ? expanded : reflected;
            values[worst] = std::min(fe, fr);
        } else if (fr < values[second]) {
            simplex[worst] = reflected;
            values[worst] = fr;
        } else {
            const auto contracted = blend(centroid, simplex[worst], 0.5);
            const double fc = f(contracted);
            if (fc < values[worst]) {
                simplex[worst] = contracted;
                values[worst] = fc;
            } else {
                for (size_t k = 0; k <= n; ++k)
                    if (k != best) {
                        simplex[k] = blend(simplex[best], simplex[k], 0.5);
                        values[k] = f(simplex[k]);
                    }
            }
        }
    }
    return simplex[static_cast<size_t>(std::min_element(values.begin(), values.end()) - values.begin())];
}

// One table entry: the inverse LTC matrix, scaled so its m11 is 1 (the integral is scale-invariant), and the albedo.
struct Entry {
    double m00 = 1.0, m02 = 0.0, m20 = 0.0, m22 = 1.0;
    double scale = 0.0, bias = 0.0;
};

// Row a of a size x size table (roughness a / (size - 1)), view angles from normal incidence out, each fit starting
// from the last.
void fitRoughness(int a, int size, std::vector<Entry>& table)
{
    const double roughness = std::max(kMinRoughness, static_cast<double>(a) / (size - 1));
    std::array<double, 3> params{roughness * roughness, roughness * roughness, 0.0};
    for (int t = 0; t < size; ++t) {
        // The table's v axis is sqrt(1 - N.V), denser toward grazing angles; grazing itself is held just short of 90.
        const double x = static_cast<double>(t) / (size - 1);
        const double theta = std::min(1.57, std::acos(1.0 - x * x));
        const Vec3 v{std::sin(theta), 0.0, std::cos(theta)};
        const Moments lobe = moments(v, roughness);
        const auto error = [&](const std::array<double, 3>& p) { return fitError(p, v, roughness, lobe); };
        if (t == 0) { // isotropic at normal incidence: one scale
            const auto iso = [&](const std::array<double, 3>& p) { return error({p[0], p[0], 0.0}); };
            params = minimize(iso, params, {0});
            params = {params[0], params[0], 0.0};
        } else {
            params = minimize(error, params, {0, 1, 2});
        }
        const Mat3 inv = inverse(ltcMatrix(params, lobe.average));
        Entry& e = table[static_cast<size_t>(t * size + a)];
        e = {inv[0][0] / inv[1][1], inv[0][2] / inv[1][1], inv[2][0] / inv[1][1], inv[2][2] / inv[1][1], lobe.scale,
             lobe.bias};
    }
}

std::vector<Entry> fitTable(int size)
{
    std::vector<Entry> table(static_cast<size_t>(size * size));
    std::vector<std::thread> threads;
    for (int a = 0; a < size; ++a)
        threads.emplace_back(fitRoughness, a, size, std::ref(table));
    for (std::thread& thread : threads)
        thread.join();
    return table;
}

// --check: how far the table's specular (fs_mesh.sc areaShade with F0 = 1) is from the BRDF integrated over rects
// directly, for a spread of roughnesses, view angles and rects.

// The table bilinearly, as fs_mesh.sc ltcLookup.
Entry lookup(const std::vector<Entry>& table, int size, double roughness, double nv)
{
    const double u = roughness * (size - 1), w = std::sqrt(1.0 - nv) * (size - 1);
    const int a = std::min(static_cast<int>(u), size - 2), t = std::min(static_cast<int>(w), size - 2);
    const double fu = u - a, fw = w - t;
    const auto at = [&](int ta, int tt) { return table[static_cast<size_t>(tt * size + ta)]; };
    const auto mix = [](const Entry& p, const Entry& q, double s) {
        return Entry{p.m00 + (q.m00 - p.m00) * s, p.m02 + (q.m02 - p.m02) * s, p.m20 + (q.m20 - p.m20) * s,
                     p.m22 + (q.m22 - p.m22) * s, p.scale + (q.scale - p.scale) * s, p.bias + (q.bias - p.bias) * s};
    };
    return mix(mix(at(a, t), at(a + 1, t), fu), mix(at(a, t + 1), at(a + 1, t + 1), fu), fw);
}

// fs_mesh.sc ltcIntegrate in its frame (normal +Z, view in XZ): corners transformed by the entry's inverse matrix,
// clipped to the horizon, the clamped cosine integrated over them.
double ltcIntegral(const Entry& e, const std::array<Vec3, 4>& corners)
{
    const auto edge = [](Vec3 a, Vec3 b) {
        a = normalize(a);
        b = normalize(b);
        const double x = std::clamp(dot(a, b), -1.0, 1.0);
        const double ratio = x > 0.9999999 ? 1.0 : std::acos(x) / std::sqrt(1.0 - x * x);
        return (a.x * b.y - a.y * b.x) * ratio;
    };
    std::array<Vec3, 4> q;
    for (size_t i = 0; i < 4; ++i)
        q[i] = {e.m00 * corners[i].x + e.m02 * corners[i].z, corners[i].y, e.m20 * corners[i].x + e.m22 * corners[i].z};
    double sum = 0.0;
    Vec3 exit, entry;
    bool clipped = false;
    for (size_t i = 0; i < 4; ++i) {
        const Vec3 a = q[i], b = q[(i + 1) % 4];
        if (a.z >= 0.0 && b.z >= 0.0) {
            sum += edge(a, b);
        } else if (a.z >= 0.0 || b.z >= 0.0) {
            const Vec3 cut = a + (b - a) * (a.z / (a.z - b.z));
            clipped = true;
            if (a.z >= 0.0) {
                sum += edge(a, cut);
                exit = cut;
            } else {
                sum += edge(cut, b);
                entry = cut;
            }
        }
    }
    if (clipped)
        sum += edge(exit, entry);
    return std::max(sum / (2.0 * kPi), 0.0);
}

// The BRDF times N.L over the rect, by stratified samples of its area (solid angle = cos at the rect * area / d^2).
double bruteForce(Vec3 v, double roughness, const std::array<Vec3, 4>& corners)
{
    constexpr int n = 256;
    const Vec3 u = corners[1] - corners[0], w = corners[3] - corners[0];
    const Vec3 normal{u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x}; // length: the area
    const double area = length(normal);
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            const Vec3 p = corners[0] + u * ((i + 0.5) / n) + w * ((j + 0.5) / n);
            const double d2 = dot(p, p);
            const Vec3 l = p * (1.0 / std::sqrt(d2));
            sum += brdfCos(v, l, roughness) * std::abs(dot(l, normal)) / area / d2;
        }
    return sum * area / (static_cast<double>(n) * n);
}

void check(const std::vector<Entry>& table, int size, bool verbose)
{
    // Rects facing the shading point at the origin, corners clockwise as seen from it: overhead, off to the side and
    // tilted, large and near, and low toward grazing reflections.
    const std::array<Vec3, 4> rects[] = {
        {Vec3{-0.5, -0.5, 2.0}, Vec3{0.5, -0.5, 2.0}, Vec3{0.5, 0.5, 2.0}, Vec3{-0.5, 0.5, 2.0}},
        {Vec3{-2.5, -0.5, 1.0}, Vec3{-1.5, -0.5, 1.5}, Vec3{-1.5, 0.5, 1.5}, Vec3{-2.5, 0.5, 1.0}},
        {Vec3{-2.0, -2.0, 1.0}, Vec3{2.0, -2.0, 1.0}, Vec3{2.0, 2.0, 1.0}, Vec3{-2.0, 2.0, 1.0}},
        {Vec3{-3.0, -0.5, 0.2}, Vec3{-3.0, -0.5, 1.2}, Vec3{-3.0, 0.5, 1.2}, Vec3{-3.0, 0.5, 0.2}},
    };
    // Errors as a share of the whole lobe (its albedo: what a rect filling the hemisphere would reflect), and relative
    // to the brute force value where that is at least a tenth of the lobe.
    double sumShare = 0.0, maxShare = 0.0, sumRelative = 0.0, maxRelative = 0.0;
    int count = 0, relativeCount = 0;
    for (const double roughness : {0.1, 0.2, 0.3, 0.5, 0.75, 1.0}) // lower: too sharp for 256 x 256 brute force samples
        for (const double degrees : {0.0, 30.0, 60.0, 80.0})
            for (const auto& rect : rects) {
                const double theta = degrees * kPi / 180.0;
                const Vec3 v{std::sin(theta), 0.0, std::cos(theta)};
                const double truth = bruteForce(v, roughness, rect);
                const Entry e = lookup(table, size, roughness, v.z);
                const double albedo = e.scale + e.bias;
                const double ltc = albedo * ltcIntegral(e, rect);
                const double share = std::abs(ltc - truth) / albedo;
                sumShare += share;
                maxShare = std::max(maxShare, share);
                ++count;
                if (truth >= 0.1 * albedo) {
                    const double relative = std::abs(ltc - truth) / truth;
                    sumRelative += relative;
                    maxRelative = std::max(maxRelative, relative);
                    ++relativeCount;
                }
                if (verbose)
                    std::printf("  roughness %.2f, view %2.0f deg, rect %d: brute force %.5f, table %.5f (albedo %.5f)\n",
                                roughness, degrees, static_cast<int>(&rect - rects), truth, ltc, albedo);
            }
    std::printf("%dx%d table, %d cases: error / albedo mean %.2f%%, max %.2f%%; relative error (%d cases with a tenth of "
                "the albedo or more) mean %.1f%%, max %.1f%%\n",
                size, size, count, 100.0 * sumShare / count, 100.0 * maxShare, relativeCount,
                100.0 * sumRelative / relativeCount, 100.0 * maxRelative);
}

} // namespace

int main(int argc, char** argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "--check" || mode == "--check-verbose") {
        const bool verbose = mode == "--check-verbose";
        for (const int size : {32, kSize, 8})
            check(fitTable(size), size, verbose);
        return 0;
    }
    const bool verify = mode == "--verify";
    if (argc != (verify ? 3 : 2)) {
        std::fprintf(stderr, "Usage: kast_ltc_fit <out ltc_table.sh> | --verify <ltc_table.sh> | --check | --check-verbose\n");
        return 2;
    }
    const std::vector<Entry> table = fitTable(kSize);

    std::string text =
        "// Generated by tools/ltc_fit (kast_ltc_fit): do not edit. The Linearly Transformed Cosines (Heitz et al. 2016)\n"
        "// fit of fs_mesh.sc's GGX specular, LTC_SIZE x LTC_SIZE entries: row t (sqrt(1 - N.V) = t / (LTC_SIZE - 1)),\n"
        "// column a (roughness a / (LTC_SIZE - 1)), two vec4 each: the inverse LTC matrix (m00, m02, m20, m22; m11 = 1,\n"
        "// view in the XZ plane, normal +Z), then the BRDF albedo with Fresnel F0 * x + y. Included by fs_mesh.sc and\n"
        "// renderer.cpp inside an array initializer.\n";
    // A float literal both HLSL and C++ read, its exponent without leading zeros (bgfx's shader preprocessor takes
    // "e-08" for an octal number).
    const auto number = [](double value) {
        std::string s = std::format("{:.7e}", value);
        const size_t e = s.find('e') + 2; // past the sign
        const size_t digits = s.find_first_not_of('0', e);
        s.erase(e, std::min(digits, s.size() - 1) - e);
        return s + "f";
    };
    for (const Entry& e : table)
        text += std::format("{}, {}, {}, {}, {}, {}, 0.0f, 0.0f,\n", number(e.m00), number(e.m02), number(e.m20),
                            number(e.m22), number(e.scale), number(e.bias));
    const char* path = argv[verify ? 2 : 1];
    if (verify) { // the checked-in table is what this source fits
        std::ifstream in(path, std::ios::binary);
        std::string existing{std::istreambuf_iterator<char>(in), {}};
        std::erase(existing, '\r'); // a checkout may have made the line ends CRLF
        if (existing != text) {
            std::fprintf(stderr, "%s is not what kast_ltc_fit writes: regenerate it (tools/ltc_fit/README.md)\n", path);
            return 1;
        }
        std::printf("%s is up to date\n", path);
        return 0;
    }
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) {
        std::fprintf(stderr, "Cannot write %s\n", path);
        return 1;
    }
    std::printf("Wrote %s: %dx%d LTC entries\n", path, kSize, kSize);
    return 0;
}
