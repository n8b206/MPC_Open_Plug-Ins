// Bloom Voice - a 16-band vocoder by N8b, made to sing Chord Bloom's chords with your mic.
//
// The MPC gives instruments no audio input and effects no MIDI, so the two meet on a submix:
//   * Chord Bloom (Output = "Vocoder L") sends its chords on the LEFT channel only.
//   * The mic's audio track is panned hard RIGHT into the same submix.
//   * Bloom Voice on that submix uses the left side as the carrier (the chords) and the
//     right side as the voice, and outputs the vocoded result in stereo.
// "Inputs" swaps the two sides if they arrive the other way round.

#include "mpc_plugin.h"
#include "dsp/svf.h"

#include <cmath>

using mpc::dsp::SVF;

namespace {
enum ParamIndex
{
    kMix = 0, kMicGain, kFormant, kRelease, kSibilance, kBreath, kSynthThru, kGate, kInputs, kOutput,
    kNumParams
};
constexpr int kBands = 16;

struct Follower
{
    float env = 0.0f, att = 0.0f, rel = 0.0f;
    void set(float attMs, float relMs, float sr)
    {
        att = 1.0f - std::exp(-1.0f / (0.001f * attMs * sr));
        rel = 1.0f - std::exp(-1.0f / (0.001f * relMs * sr));
    }
    float process(float x) { const float a = std::fabs(x); env += (a > env ? att : rel) * (a - env); return env; }
};
} // namespace

class BloomVoice : public mpc::Plugin
{
public:
    BloomVoice() : mpc::Plugin(makeInfo())
    {
        addParam(mpc::floatParam("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
        addParam(mpc::floatParam("micgain", "Mic Gain", -12.0f, 24.0f, 6.0f, "dB", 1));
        addParam(mpc::floatParam("formant", "Formant", -12.0f, 12.0f, 0.0f, "st", 1));
        addParam(mpc::logParam("release", "Release", 5.0f, 400.0f, 40.0f, "ms", 0));
        addParam(mpc::floatParam("sib", "Sibilance", 0.0f, 100.0f, 40.0f, "%", 0));
        addParam(mpc::floatParam("breath", "Breath", 0.0f, 100.0f, 10.0f, "%", 0));
        addParam(mpc::floatParam("thru", "Synth Thru", 0.0f, 100.0f, 0.0f, "%", 0));
        addParam(mpc::floatParam("gate", "Mic Gate", -80.0f, -20.0f, -60.0f, "dB", 0));
        addParam(mpc::choiceParam("inputs", "Inputs", {"Syn L/Mic R", "Mic L/Syn R"}, 0));
        addParam(mpc::floatParam("output", "Output", -24.0f, 12.0f, 0.0f, "dB", 1));

        addProgram("Classic Robot", {100, 6, 0, 40, 40, 10, 0, -60, 0, 0});
        addProgram("Choir Bloom", {100, 6, -3, 120, 25, 0, 10, -60, 0, 0});
        addProgram("Bright Talker", {100, 9, 4, 20, 60, 20, 0, -55, 0, 0});
        addProgram("Half and Half", {55, 6, 0, 50, 40, 10, 0, -60, 0, 0});
    }

    bool formatValue(int index, float plain, std::string& out) override
    {
        const mpc::Param& pr = param(index);
        if (!pr.unit.empty() && pr.kind == mpc::Param::Kind::Float)
        {
            out = pr.display(plain) + (pr.unit == "%" ? "" : " ") + pr.unit;
            return true;
        }
        return false;
    }

    void prepare(double sampleRate, int) override
    {
        sr_ = (float)sampleRate;
        for (int b = 0; b < kBands; ++b)
        {
            for (int s = 0; s < 2; ++s) { mod_[b][s].setSampleRate(sampleRate); car_[b][s].setSampleRate(sampleRate); }
            const float t = (float)b / (kBands - 1);
            freq_[b] = 110.0f * std::pow(7600.0f / 110.0f, t);
            panL_[b] = (b & 1) ? 0.55f : 0.85f;
            panR_[b] = (b & 1) ? 0.85f : 0.55f;
        }
        sib_.setSampleRate(sampleRate); sib_.set(5500.0f, 0.1f);
        micHp_.setSampleRate(sampleRate); micHp_.set(80.0f, 0.1f);
        lastFormant_ = 999.0f;
        lastRelease_ = -1.0f;
        reset();
    }

    void reset() override
    {
        for (int b = 0; b < kBands; ++b) { for (int s = 0; s < 2; ++s) { mod_[b][s].reset(); car_[b][s].reset(); } fol_[b].env = 0.0f; }
        sib_.reset(); micHp_.reset();
        gateEnv_ = 0.0f;
        mixS_ = get(kMix) * 0.01f;
    }

    void process(mpc::ProcessContext& ctx) override
    {
        const int n = ctx.numFrames;
        if (ctx.numOutputs < 1) return;
        float* oL = ctx.outputs[0];
        float* oR = ctx.numOutputs > 1 ? ctx.outputs[1] : nullptr;
        const float* iL = (ctx.inputs && ctx.numInputs > 0) ? ctx.inputs[0] : nullptr;
        const float* iR = (ctx.inputs && ctx.numInputs > 1) ? ctx.inputs[1] : iL;
        if (!iL) { for (int i = 0; i < n; ++i) { oL[i] = 0.0f; if (oR) oR[i] = 0.0f; } return; }

        const bool swap = getInt(kInputs) == 1;
        const float micGain = std::pow(10.0f, get(kMicGain) / 20.0f);
        const float mixT = get(kMix) * 0.01f;
        const float sib = get(kSibilance) * 0.01f;
        const float breath = get(kBreath) * 0.01f;
        const float thru = get(kSynthThru) * 0.01f;
        const float gateLin = std::pow(10.0f, get(kGate) / 20.0f);
        const float outGain = std::pow(10.0f, get(kOutput) / 20.0f);

        const float formant = get(kFormant);
        if (formant != lastFormant_)
        {
            lastFormant_ = formant;
            const float shift = std::pow(2.0f, formant / 12.0f);
            for (int b = 0; b < kBands; ++b)
            {
                for (int s = 0; s < 2; ++s)
                {
                    mod_[b][s].set(freq_[b], kRes);
                    car_[b][s].set(mpc::clamp(freq_[b] * shift, 40.0f, 18000.0f), kRes);
                }
            }
        }
        const float rel = get(kRelease);
        if (rel != lastRelease_)
        {
            lastRelease_ = rel;
            for (auto& f : fol_) f.set(1.5f, rel, sr_);
            gateFol_.set(1.0f, 80.0f, sr_);
        }

        for (int i = 0; i < n; ++i)
        {
            // Read both inputs first: processing is in place.
            const float a = iL[i], b = iR[i];
            const float synth = swap ? b : a;
            float mic = (swap ? a : b) * micGain;
            mic = micHp_.process(mic, SVF::Mode::Highpass);

            // Simple gate so room noise doesn't open the bands.
            const float g = gateFol_.process(mic);
            gateEnv_ += ((g > gateLin ? 1.0f : 0.0f) - gateEnv_) * 0.002f;

            // Breath: a little noise in the carrier so consonants come through.
            rng_ = rng_ * 1664525u + 1013904223u;
            const float noise = ((float)(rng_ >> 9) * (1.0f / 4194304.0f) - 1.0f) * 0.25f;
            const float carrier = synth + noise * breath * std::fabs(synth + 0.02f) * 4.0f + noise * breath * 0.05f;

            float l = 0.0f, r = 0.0f;
            for (int k = 0; k < kBands; ++k)
            {
                float lp, bp, hp;
                mod_[k][0].processAll(mic, lp, bp, hp);
                mod_[k][1].processAll(bp * kNorm, lp, bp, hp);
                const float env = fol_[k].process(bp * kNorm);
                car_[k][0].processAll(carrier, lp, bp, hp);
                car_[k][1].processAll(bp * kNorm, lp, bp, hp);
                const float v = bp * kNorm * env;
                l += v * panL_[k];
                r += v * panR_[k];
            }
            const float makeup = kMakeup * gateEnv_;
            l *= makeup; r *= makeup;
            const float s = sib_.process(mic, SVF::Mode::Highpass) * sib * gateEnv_;
            l += s; r += s;

            mixS_ += (mixT - mixS_) * 0.002f;
            const float dry = mic * 0.7f;
            float outL = dry + (l - dry) * mixS_ + synth * thru;
            float outR = dry + (r - dry) * mixS_ + synth * thru;
            outL = mpc::dsp::fastTanh(outL * outGain);
            outR = mpc::dsp::fastTanh(outR * outGain);
            oL[i] = outL;
            if (oR) oR[i] = outR;
        }
    }

private:
    static mpc::PluginInfo makeInfo()
    {
        mpc::PluginInfo info;
        info.name = "Bloom Voice";
        info.vendor = "N8b";
        info.uniqueId = MPC_FOURCC('N', '8', 'B', 'v');
        info.version = 1000;
        info.isInstrument = false;
        info.numInputs = 2;
        info.numOutputs = 2;
        info.tailSamples = 1;
        return info;
    }

    static constexpr float kMakeup = 150.0f;   // 16 narrow bands lose a lot of level
    static constexpr float kRes = 0.93f;
    static constexpr float kNorm = 2.0f - 1.98f * kRes;   // bandpass peak gain is 1/k: bring it back to 1
    SVF mod_[kBands][2], car_[kBands][2], sib_, micHp_;
    Follower fol_[kBands], gateFol_;
    float freq_[kBands] = {0}, panL_[kBands] = {0}, panR_[kBands] = {0};
    float sr_ = 44100.0f, lastFormant_ = 999.0f, lastRelease_ = -1.0f, gateEnv_ = 0.0f, mixS_ = 1.0f;
    uint32_t rng_ = 0x2468ace1u;
};

MPC_PLUGIN_ENTRY(BloomVoice)
