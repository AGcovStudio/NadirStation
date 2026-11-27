// Short-range cloud-top forecast ("nowcast") from two consecutive GK-2A IR10.5 full disks, 10 minutes
// apart: motion by block matching, semi-Lagrangian advection, and a small neural network trained on
// this station's own reception that corrects what plain advection misses (clouds growing, decaying,
// smearing). Plain C++, no Qt; the model is a JSON file of weights (tools/nowcast/train.py writes it),
// so the application stays offline.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nadirstation::nowcast {

// A brightness temperature field in kelvin, NaN where there is no data. Square, row-major.
struct Field {
    int size = 0;
    std::vector<float> k;
    bool empty() const { return size <= 0 || k.empty(); }
    float at(int y, int x) const { return k[static_cast<std::size_t>(y) * size + x]; }
};

// From the 0.01 K PNG raster (0 = no data).
Field fromRaw16(const std::uint16_t* data, int size);
std::vector<std::uint16_t> toRaw16(const Field& f);

struct Motion {
    int size = 0;
    std::vector<float> vy, vx;     // displacement per step (one step = the input interval), pixels
    double goodBlocks = 0.0;       // share of blocks with enough texture to match
};

Motion estimateMotion(const Field& earlier, const Field& later);

// Value at x after `steps` steps comes from x - v * steps (bilinear; NaN when any neighbour is NaN).
Field advect(const Field& f, const Motion& m, double steps);

struct Model {
    std::vector<float> mean, scale;                 // feature standardisation
    std::vector<std::vector<float>> weights;        // layer l: in x out, row-major
    std::vector<std::vector<float>> biases;
    std::string name;
    bool valid() const { return !weights.empty() && weights.size() == biases.size() && mean.size() == scale.size(); }
};

// Reads the JSON the training script writes. Returns an invalid model on any problem.
Model loadModel(const std::string& path, std::string* error = nullptr);

// One step ahead: advection, plus the network's correction when the model is valid.
Field step(const Field& previous, const Field& now, const Motion& m, const Model& model, int threads = 4);

// `steps` steps ahead, feeding each prediction back in. progress(k) is called after step k.
std::vector<Field> forecast(const Field& previous, const Field& now, const Motion& m, const Model& model,
                            int steps, int threads = 4, void (*progress)(int, void*) = nullptr, void* user = nullptr);

}  // namespace nadirstation::nowcast
