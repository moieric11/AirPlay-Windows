#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>

namespace ap::video {
// Native pipeline counters. No CUDA synchronization or per-frame disk IO.
// Only the processing thread (or stop(), after joining it) writes the file.
class FaceSwapCadence {
public:
    enum Event { Input, Submitted, Started, Completed, Taken, Presented, Render,
        PendingReplaced, OutputReplaced, Expired, WrongSize, Held, EventCount };
    enum Time { Queue, Prepare, RoundTrip, Copy, Idle, ResultHold, RenderWait,
        PresentWait, OriginAge, TimeCount };
    using Clock=std::chrono::steady_clock;
    void open(const std::filesystem::path& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        file_.close(); file_.clear();
        events_={}; times_={}; source_valid_=false; decoded_=previous_decoded_=0;
        replaced_=previous_replaced_=0; start_=last_=Clock::now();
        if (path.empty()) return;
        file_.open(path);
        file_.imbue(std::locale::classic());
        if (file_) file_ << "elapsed_s,window_s,decoded_fps,input_fps,submitted_fps,started_fps,completed_fps,taken_fps,presented_fps,render_fps,decoder_replaced,pending_replaced,output_replaced,expired,wrong_size,held,queue_ms,prepare_ms,roundtrip_ms,copy_ms,idle_ms,result_hold_ms,render_wait_ms,present_wait_ms,origin_age_ms,present_max_ms,result_hold_max_ms\n";
    }
    void event(Event e) { std::lock_guard<std::mutex> lock(mutex_); ++events_[e]; }
    void timing(Time t,double ms) {
        std::lock_guard<std::mutex> lock(mutex_); times_[t].add(ms);
    }
    void renderer(uint64_t decoded,uint64_t replaced,bool input,bool presented,bool held,
                  double wait_ms,double present_ms,double origin_age_ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!source_valid_) {
            previous_decoded_=decoded; previous_replaced_=replaced; source_valid_=true;
        }
        if (decoded<decoded_) previous_decoded_=decoded;
        if (replaced<replaced_) previous_replaced_=replaced;
        decoded_=decoded; replaced_=replaced;
        ++events_[Render]; events_[Input]+=input; events_[Presented]+=presented; events_[Held]+=held;
        times_[RenderWait].add(wait_ms); times_[PresentWait].add(present_ms);
        if (presented && origin_age_ms>=0) times_[OriginAge].add(origin_age_ms);
    }
    void flush(bool force=false,Clock::time_point now=Clock::now()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open()) return;
        const double seconds=std::chrono::duration<double>(now-last_).count();
        if ((!force && seconds<2) || seconds<.001) return;
        file_ << std::fixed << std::setprecision(3)
              << std::chrono::duration<double>(now-start_).count() << ',' << seconds
              << ',' << (decoded_-previous_decoded_)/seconds;
        for (auto e : {Input,Submitted,Started,Completed,Taken,Presented,Render}) file_ << ',' << events_[e]/seconds;
        file_ << ',' << replaced_-previous_replaced_;
        for (auto e : {PendingReplaced,OutputReplaced,Expired,WrongSize,Held}) file_ << ',' << events_[e];
        for (const auto& t:times_) file_ << ',' << (t.count ? t.sum/t.count : 0);
        file_ << ',' << times_[PresentWait].max << ',' << times_[ResultHold].max << '\n';
        file_.flush(); events_={}; times_={};
        previous_decoded_=decoded_; previous_replaced_=replaced_; last_=now;
    }
    void close() { flush(true); std::lock_guard<std::mutex> lock(mutex_); file_.close(); }
private:
    struct Timing {
        uint64_t count=0; double sum=0,max=0;
        void add(double value) { ++count; sum+=value; if (value>max) max=value; }
    };
    std::mutex mutex_;
    std::ofstream file_;
    std::array<uint64_t,EventCount> events_{};
    std::array<Timing,TimeCount> times_{};
    bool source_valid_=false;
    uint64_t decoded_=0,previous_decoded_=0,replaced_=0,previous_replaced_=0;
    Clock::time_point start_{},last_{};
};
} // namespace ap::video
