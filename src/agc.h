// 簡易 AGC（自動ゲイン調整）。Krisp のノイズ除去後にかけ、発話音量を一定に整える。
// Discord の「音量調節の自動化」に相当。
//
// 設計方針（ポンピング回避）:
//  - レベル推定は「RMSの緩やかな追従」。上昇(loud)は速め、下降(quiet/無音)は遅め。
//  - 目標ゲインは推定レベルから算出し、フレーム間でさらにゆっくり移動。
//  - 無音区間はレベルを下げず（＝ゲインを持ち上げず）、ノイズ増幅とポンピングを防ぐ。
//  - ゲインはフレーム内で線形ランプ適用し、境界の段差を作らない。
//  - 最後にソフトリミッタでクリップ回避。
#pragma once
#include <cmath>
#include <cstddef>
#include <algorithm>

class Agc {
public:
    explicit Agc(int sampleRate = 48000, float targetRms = 0.12f)
        : sr_(sampleRate), target_(targetRms) {}

    void reset() { gain_ = 1.0f; level_ = target_; }
    void setTarget(float t) { target_ = t; }
    void setEnabled(bool e) { enabled_ = e; }
    bool enabled() const { return enabled_; }
    float gain() const { return gain_; }

    // frame をその場で加工する。連続呼び出しで状態を保持する。
    void process(float* x, size_t n) {
        if (!enabled_ || n == 0) return;

        // フレーム RMS
        double s = 0;
        for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
        float rms = (float)std::sqrt(s / n);

        // レベル推定（上昇速め・下降遅め）。無音時は更新しない＝ゲイン据え置き。
        if (rms > silence_) {
            float coef = (rms > level_) ? riseLvl_ : fallLvl_;
            level_ += coef * (rms - level_);
        }
        float lvl = std::max(level_, silence_);

        // 目標ゲイン（発話がある時だけ更新）
        float targetGain = gain_;
        if (rms > silence_) {
            targetGain = target_ / lvl;
            targetGain = std::min(std::max(targetGain, minGain_), maxGain_);
        }

        // フレーム間でゲインをゆっくり移動（下げは少し速く、上げは遅く）
        float startGain = gain_;
        float gc = (targetGain < gain_) ? downGain_ : upGain_;
        gain_ += gc * (targetGain - gain_);
        float endGain = gain_;

        // フレーム内で startGain→endGain へ線形ランプ適用（段差回避）＋ソフトリミッタ
        const float knee = 0.90f;
        for (size_t i = 0; i < n; i++) {
            float g = startGain + (endGain - startGain) * ((float)i / (float)n);
            float y = x[i] * g;
            if (y > knee)       y = knee + (1.0f - knee) * std::tanh((y - knee) / (1.0f - knee));
            else if (y < -knee) y = -knee + (1.0f - knee) * std::tanh((y + knee) / (1.0f - knee));
            x[i] = y;
        }
    }

private:
    int   sr_;
    float target_;
    bool  enabled_ = true;
    float gain_  = 1.0f;
    float level_ = 0.12f;

    // フレーム(10ms)単位の追従係数（0..1、小さいほど遅い）
    float riseLvl_ = 0.20f;   // レベル上昇追従（約50ms）
    float fallLvl_ = 0.03f;   // レベル下降追従（約330ms）ゆっくり＝安定
    float upGain_   = 0.05f;  // ゲイン上げ（小声を持ち上げる。約200ms）遅め＝ポンピング回避
    float downGain_ = 0.15f;  // ゲイン下げ（突発の大音量を抑える。約65ms）
    float maxGain_ = 6.0f;    // 最大 +15.6dB
    float minGain_ = 0.3f;    // 最小 -10.5dB
    float silence_ = 0.006f;  // これ未満は無音扱い（ゲイン据え置き）
};
