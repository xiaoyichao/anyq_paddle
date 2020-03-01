/* Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#ifdef PADDLE_WITH_BOX_PS
#include <boxps_public.h>
#endif
#include <glog/logging.h>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>  // NOLINT
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "paddle/fluid/framework/data_set.h"
#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/scope.h"
#include "paddle/fluid/platform/gpu_info.h"
#include "paddle/fluid/platform/place.h"
#include "paddle/fluid/platform/timer.h"
#include "paddle/fluid/string/string_helper.h"

namespace paddle {
namespace framework {

#ifdef PADDLE_WITH_BOX_PS
class BasicAucCalculator {
 public:
  BasicAucCalculator() {}
  void init(int table_size) { set_table_size(table_size); }
  void reset() {
    for (int i = 0; i < 2; i++) {
      _table[i].assign(_table_size, 0.0);
    }
    _local_abserr = 0;
    _local_sqrerr = 0;
    _local_pred = 0;
  }
  void add_data(double pred, int label) {
    PADDLE_ENFORCE_GE(pred, 0.0, platform::errors::PreconditionNotMet(
                                     "pred should be greater than 0"));
    PADDLE_ENFORCE_LE(pred, 1.0, platform::errors::PreconditionNotMet(
                                     "pred should be lower than 1"));
    PADDLE_ENFORCE_EQ(
        label * label, label,
        platform::errors::PreconditionNotMet(
            "label must be equal to 0 or 1, but its value is: %d", label));
    int pos = std::min(static_cast<int>(pred * _table_size), _table_size - 1);
    PADDLE_ENFORCE_GE(
        pos, 0,
        platform::errors::PreconditionNotMet(
            "pos must be equal or greater than 0, but its value is: %d", pos));
    PADDLE_ENFORCE_LT(
        pos, _table_size,
        platform::errors::PreconditionNotMet(
            "pos must be less than table_size, but its value is: %d", pos));
    std::lock_guard<std::mutex> lock(_table_mutex);
    _local_abserr += fabs(pred - label);
    _local_sqrerr += (pred - label) * (pred - label);
    _local_pred += pred;
    _table[label][pos]++;
  }
  void compute();
  int table_size() const { return _table_size; }
  double bucket_error() const { return _bucket_error; }
  double auc() const { return _auc; }
  double mae() const { return _mae; }
  double actual_ctr() const { return _actual_ctr; }
  double predicted_ctr() const { return _predicted_ctr; }
  double size() const { return _size; }
  double rmse() const { return _rmse; }
  std::vector<double>& get_negative() { return _table[0]; }
  std::vector<double>& get_postive() { return _table[1]; }
  double& local_abserr() { return _local_abserr; }
  double& local_sqrerr() { return _local_sqrerr; }
  double& local_pred() { return _local_pred; }
  void calculate_bucket_error();

 protected:
  double _local_abserr = 0;
  double _local_sqrerr = 0;
  double _local_pred = 0;
  double _auc = 0;
  double _mae = 0;
  double _rmse = 0;
  double _actual_ctr = 0;
  double _predicted_ctr = 0;
  double _size;
  double _bucket_error = 0;

 private:
  void set_table_size(int table_size) {
    _table_size = table_size;
    for (int i = 0; i < 2; i++) {
      _table[i] = std::vector<double>();
    }
    reset();
  }
  int _table_size;
  std::vector<double> _table[2];
  static constexpr double kRelativeErrorBound = 0.05;
  static constexpr double kMaxSpan = 0.01;
  std::mutex _table_mutex;
};

class BoxWrapper {
 public:
  virtual ~BoxWrapper() {}
  BoxWrapper() {}

  void FeedPass(int date, const std::vector<uint64_t>& feasgin_to_box) const;
  void BeginFeedPass(int date, boxps::PSAgentBase** agent) const;
  void EndFeedPass(boxps::PSAgentBase* agent) const;
  void BeginPass() const;
  void EndPass() const;
  void PullSparse(const paddle::platform::Place& place,
                  const std::vector<const uint64_t*>& keys,
                  const std::vector<float*>& values,
                  const std::vector<int64_t>& slot_lengths,
                  const int hidden_size);
  void PushSparseGrad(const paddle::platform::Place& place,
                      const std::vector<const uint64_t*>& keys,
                      const std::vector<const float*>& grad_values,
                      const std::vector<int64_t>& slot_lengths,
                      const int hidden_size, const int batch_size);
  void CopyForPull(const paddle::platform::Place& place, uint64_t** gpu_keys,
                   const std::vector<float*>& values,
                   const boxps::FeatureValueGpu* total_values_gpu,
                   const int64_t* gpu_len, const int slot_num,
                   const int hidden_size, const int64_t total_length);
  void CopyForPush(const paddle::platform::Place& place,
                   const std::vector<const float*>& grad_values,
                   boxps::FeaturePushValueGpu* total_grad_values_gpu,
                   const std::vector<int64_t>& slot_lengths,
                   const int hidden_size, const int64_t total_length,
                   const int batch_size);
  void CopyKeys(const paddle::platform::Place& place, uint64_t** origin_keys,
                uint64_t* total_keys, const int64_t* gpu_len, int slot_num,
                int total_len);
  boxps::PSAgentBase* GetAgent() { return p_agent_; }
  void InitializeGPU(const char* conf_file, const std::vector<int>& slot_vector,
                     const std::vector<std::string>& slot_omit_in_feedpass) {
    if (nullptr != s_instance_) {
      VLOG(3) << "Begin InitializeGPU";
      std::vector<cudaStream_t*> stream_list;
      for (int i = 0; i < platform::GetCUDADeviceCount(); ++i) {
        VLOG(3) << "before get context i[" << i << "]";
        platform::CUDADeviceContext* context =
            dynamic_cast<platform::CUDADeviceContext*>(
                platform::DeviceContextPool::Instance().Get(
                    platform::CUDAPlace(i)));
        stream_list_[i] = context->stream();
        stream_list.push_back(&stream_list_[i]);
      }
      VLOG(2) << "Begin call InitializeGPU in BoxPS";
      // the second parameter is useless
      s_instance_->boxps_ptr_->InitializeGPU(conf_file, -1, stream_list);
      p_agent_ = boxps::PSAgentBase::GetIns(feedpass_thread_num_);
      p_agent_->Init();
      for (const auto& slot_name : slot_omit_in_feedpass) {
        slot_name_omited_in_feedpass_.insert(slot_name);
      }
      slot_vector_ = slot_vector;
      keys_tensor.resize(platform::GetCUDADeviceCount());
    }
  }

  int GetFeedpassThreadNum() const { return feedpass_thread_num_; }

  void Finalize() {
    VLOG(3) << "Begin Finalize";
    if (nullptr != s_instance_) {
      s_instance_->boxps_ptr_->Finalize();
    }
  }

  const std::string SaveBase(const char* batch_model_path,
                             const char* xbox_model_path) {
    VLOG(3) << "Begin SaveBase";
    std::string ret_str;
    int ret = boxps_ptr_->SaveBase(batch_model_path, xbox_model_path, ret_str);
    PADDLE_ENFORCE_EQ(ret, 0, platform::errors::PreconditionNotMet(
                                  "SaveBase failed in BoxPS."));
    return ret_str;
  }

  const std::string SaveDelta(const char* xbox_model_path) {
    VLOG(3) << "Begin SaveDelta";
    std::string ret_str;
    int ret = boxps_ptr_->SaveDelta(xbox_model_path, ret_str);
    PADDLE_ENFORCE_EQ(ret, 0, platform::errors::PreconditionNotMet(
                                  "SaveDelta failed in BoxPS."));
    return ret_str;
  }

  static std::shared_ptr<BoxWrapper> GetInstance() {
    if (nullptr == s_instance_) {
      // If main thread is guaranteed to init this, this lock can be removed
      static std::mutex mutex;
      std::lock_guard<std::mutex> lock(mutex);
      if (nullptr == s_instance_) {
        VLOG(3) << "s_instance_ is null";
        s_instance_.reset(new paddle::framework::BoxWrapper());
        s_instance_->boxps_ptr_.reset(boxps::BoxPSBase::GetIns());
      }
    }
    return s_instance_;
  }

  const std::unordered_set<std::string>& GetOmitedSlot() const {
    return slot_name_omited_in_feedpass_;
  }

  class MetricMsg {
   public:
    MetricMsg() {}
    MetricMsg(const std::string& label_varname, const std::string& pred_varname,
              int is_join, int bucket_size = 1000000)
        : label_varname_(label_varname),
          pred_varname_(pred_varname),
          is_join_(is_join) {
      calculator = new BasicAucCalculator();
      calculator->init(bucket_size);
    }
    virtual ~MetricMsg() {}

    int IsJoin() const { return is_join_; }
    BasicAucCalculator* GetCalculator() { return calculator; }
    virtual void add_data(const Scope* exe_scope) {
      std::vector<int64_t> label_data;
      get_data<int64_t>(exe_scope, label_varname_, &label_data);
      std::vector<float> pred_data;
      get_data<float>(exe_scope, pred_varname_, &pred_data);
      auto cal = GetCalculator();
      auto batch_size = label_data.size();
      for (size_t i = 0; i < batch_size; ++i) {
        cal->add_data(pred_data[i], label_data[i]);
      }
    }
    template <class T = float>
    static void get_data(const Scope* exe_scope, const std::string& varname,
                         std::vector<T>* data) {
      auto* var = exe_scope->FindVar(varname.c_str());
      PADDLE_ENFORCE_NOT_NULL(
          var, platform::errors::NotFound(
                   "Error: var %s is not found in scope.", varname.c_str()));
      auto& gpu_tensor = var->Get<LoDTensor>();
      auto* gpu_data = gpu_tensor.data<T>();
      auto len = gpu_tensor.numel();
      data->resize(len);
      cudaMemcpy(data->data(), gpu_data, sizeof(T) * len,
                 cudaMemcpyDeviceToHost);
    }
    static inline std::pair<int, int> parse_cmatch_rank(uint64_t x) {
      // first 32 bit store cmatch and second 32 bit store rank
      return std::make_pair(static_cast<int>(x >> 32),
                            static_cast<int>(x & 0xff));
    }

   protected:
    std::string label_varname_;
    std::string pred_varname_;
    int is_join_;
    BasicAucCalculator* calculator;
  };

  class MultiTaskMetricMsg : public MetricMsg {
   public:
    MultiTaskMetricMsg(const std::string& label_varname,
                       const std::string& pred_varname_list, int is_join,
                       const std::string& cmatch_rank_group,
                       const std::string& cmatch_rank_varname,
                       int bucket_size = 1000000) {
      label_varname_ = label_varname;
      cmatch_rank_varname_ = cmatch_rank_varname;
      is_join_ = is_join;
      calculator = new BasicAucCalculator();
      calculator->init(bucket_size);
      for (auto& cmatch_rank : string::split_string(cmatch_rank_group)) {
        const std::vector<std::string>& cur_cmatch_rank =
            string::split_string(cmatch_rank, "_");
        PADDLE_ENFORCE_EQ(
            cur_cmatch_rank.size(), 2,
            platform::errors::PreconditionNotMet(
                "illegal multitask auc spec: %s", cmatch_rank.c_str()));
        cmatch_rank_v.emplace_back(atoi(cur_cmatch_rank[0].c_str()),
                                   atoi(cur_cmatch_rank[1].c_str()));
      }
      for (const auto& pred_varname : string::split_string(pred_varname_list)) {
        pred_v.emplace_back(pred_varname);
      }
      PADDLE_ENFORCE_EQ(cmatch_rank_v.size(), pred_v.size(),
                        platform::errors::PreconditionNotMet(
                            "cmatch_rank's size [%lu] should be equal to pred "
                            "list's size [%lu], but ther are not equal",
                            cmatch_rank_v.size(), pred_v.size()));
    }
    virtual ~MultiTaskMetricMsg() {}
    void add_data(const Scope* exe_scope) override {
      std::vector<int64_t> cmatch_rank_data;
      get_data<int64_t>(exe_scope, cmatch_rank_varname_, &cmatch_rank_data);
      std::vector<int64_t> label_data;
      get_data<int64_t>(exe_scope, label_varname_, &label_data);
      size_t batch_size = cmatch_rank_data.size();
      PADDLE_ENFORCE_EQ(
          batch_size, label_data.size(),
          platform::errors::PreconditionNotMet(
              "illegal batch size: batch_size[%lu] and label_data[%lu]",
              batch_size, label_data.size()));

      std::vector<std::vector<float>> pred_data_list(pred_v.size());
      for (size_t i = 0; i < pred_v.size(); ++i) {
        get_data<float>(exe_scope, pred_v[i], &pred_data_list[i]);
      }
      for (size_t i = 0; i < pred_data_list.size(); ++i) {
        PADDLE_ENFORCE_EQ(
            batch_size, pred_data_list[i].size(),
            platform::errors::PreconditionNotMet(
                "illegal batch size: batch_size[%lu] and pred_data[%lu]",
                batch_size, pred_data_list[i].size()));
      }
      auto cal = GetCalculator();
      for (size_t i = 0; i < batch_size; ++i) {
        auto cmatch_rank_it =
            std::find(cmatch_rank_v.begin(), cmatch_rank_v.end(),
                      parse_cmatch_rank(cmatch_rank_data[i]));
        if (cmatch_rank_it != cmatch_rank_v.end()) {
          cal->add_data(pred_data_list[std::distance(cmatch_rank_v.begin(),
                                                     cmatch_rank_it)][i],
                        label_data[i]);
        }
      }
    }

   protected:
    std::vector<std::pair<int, int>> cmatch_rank_v;
    std::vector<std::string> pred_v;
    std::string cmatch_rank_varname_;
  };
  class CmatchRankMetricMsg : public MetricMsg {
   public:
    CmatchRankMetricMsg(const std::string& label_varname,
                        const std::string& pred_varname, int is_join,
                        const std::string& cmatch_rank_group,
                        const std::string& cmatch_rank_varname,
                        int bucket_size = 1000000) {
      label_varname_ = label_varname;
      pred_varname_ = pred_varname;
      cmatch_rank_varname_ = cmatch_rank_varname;
      is_join_ = is_join;
      calculator = new BasicAucCalculator();
      calculator->init(bucket_size);
      for (auto& cmatch_rank : string::split_string(cmatch_rank_group)) {
        const std::vector<std::string>& cur_cmatch_rank =
            string::split_string(cmatch_rank, "_");
        PADDLE_ENFORCE_EQ(
            cur_cmatch_rank.size(), 2,
            platform::errors::PreconditionNotMet(
                "illegal cmatch_rank auc spec: %s", cmatch_rank.c_str()));
        cmatch_rank_v.emplace_back(atoi(cur_cmatch_rank[0].c_str()),
                                   atoi(cur_cmatch_rank[1].c_str()));
      }
    }
    virtual ~CmatchRankMetricMsg() {}
    void add_data(const Scope* exe_scope) override {
      std::vector<int64_t> cmatch_rank_data;
      get_data<int64_t>(exe_scope, cmatch_rank_varname_, &cmatch_rank_data);
      std::vector<int64_t> label_data;
      get_data<int64_t>(exe_scope, label_varname_, &label_data);
      std::vector<float> pred_data;
      get_data<float>(exe_scope, pred_varname_, &pred_data);
      size_t batch_size = cmatch_rank_data.size();
      PADDLE_ENFORCE_EQ(
          batch_size, label_data.size(),
          platform::errors::PreconditionNotMet(
              "illegal batch size: cmatch_rank[%lu] and label_data[%lu]",
              batch_size, label_data.size()));
      PADDLE_ENFORCE_EQ(
          batch_size, pred_data.size(),
          platform::errors::PreconditionNotMet(
              "illegal batch size: cmatch_rank[%lu] and pred_data[%lu]",
              batch_size, pred_data.size()));
      auto cal = GetCalculator();
      for (size_t i = 0; i < batch_size; ++i) {
        const auto& cur_cmatch_rank = parse_cmatch_rank(cmatch_rank_data[i]);
        for (size_t j = 0; j < cmatch_rank_v.size(); ++j) {
          if (cmatch_rank_v[j] == cur_cmatch_rank) {
            cal->add_data(pred_data[i], label_data[i]);
            break;
          }
        }
      }
    }

   protected:
    std::vector<std::pair<int, int>> cmatch_rank_v;
    std::string cmatch_rank_varname_;
  };
  const std::vector<std::string>& GetMetricNameList() const {
    return metric_name_list_;
  }
  int PassFlag() const { return pass_flag_; }
  void FlipPassFlag() { pass_flag_ = 1 - pass_flag_; }
  std::map<std::string, MetricMsg*>& GetMetricList() { return metric_lists_; }

  void InitMetric(const std::string& method, const std::string& name,
                  const std::string& label_varname,
                  const std::string& pred_varname,
                  const std::string& cmatch_rank_varname, bool is_join,
                  const std::string& cmatch_rank_group,
                  int bucket_size = 1000000) {
    if (method == "AucCalculator") {
      metric_lists_.emplace(name, new MetricMsg(label_varname, pred_varname,
                                                is_join ? 1 : 0, bucket_size));
    } else if (method == "MultiTaskAucCalculator") {
      metric_lists_.emplace(
          name, new MultiTaskMetricMsg(label_varname, pred_varname,
                                       is_join ? 1 : 0, cmatch_rank_group,
                                       cmatch_rank_varname, bucket_size));
    } else if (method == "CmatchRankAucCalculator") {
      metric_lists_.emplace(
          name, new CmatchRankMetricMsg(label_varname, pred_varname,
                                        is_join ? 1 : 0, cmatch_rank_group,
                                        cmatch_rank_varname, bucket_size));
    } else {
      PADDLE_THROW(platform::errors::Unimplemented(
          "PaddleBox only support AucCalculator, MultiTaskAucCalculator and "
          "CmatchRankAucCalculator"));
    }
    metric_name_list_.emplace_back(name);
  }

  const std::vector<float> GetMetricMsg(const std::string& name) {
    const auto iter = metric_lists_.find(name);
    PADDLE_ENFORCE_NE(iter, metric_lists_.end(),
                      platform::errors::InvalidArgument(
                          "The metric name you provided is not registered."));
    std::vector<float> metric_return_values_(8, 0.0);
    auto* auc_cal_ = iter->second->GetCalculator();
    auc_cal_->calculate_bucket_error();
    auc_cal_->compute();
    metric_return_values_[0] = auc_cal_->auc();
    metric_return_values_[1] = auc_cal_->bucket_error();
    metric_return_values_[2] = auc_cal_->mae();
    metric_return_values_[3] = auc_cal_->rmse();
    metric_return_values_[4] = auc_cal_->actual_ctr();
    metric_return_values_[5] = auc_cal_->predicted_ctr();
    metric_return_values_[6] =
        auc_cal_->actual_ctr() / auc_cal_->predicted_ctr();
    metric_return_values_[7] = auc_cal_->size();
    auc_cal_->reset();
    return metric_return_values_;
  }

 private:
  static cudaStream_t stream_list_[8];
  static std::shared_ptr<boxps::BoxPSBase> boxps_ptr_;
  boxps::PSAgentBase* p_agent_ = nullptr;
  // TODO(hutuxian): magic number, will add a config to specify
  const int feedpass_thread_num_ = 30;  // magic number
  static std::shared_ptr<BoxWrapper> s_instance_;
  std::unordered_set<std::string> slot_name_omited_in_feedpass_;

  // Metric Related
  int pass_flag_ = 1;  // join: 1, update: 0
  std::map<std::string, MetricMsg*> metric_lists_;
  std::vector<std::string> metric_name_list_;
  std::vector<int> slot_vector_;
  std::vector<LoDTensor> keys_tensor;  // Cache for pull_sparse
};
#endif

class BoxHelper {
 public:
  explicit BoxHelper(paddle::framework::Dataset* dataset) : dataset_(dataset) {}
  virtual ~BoxHelper() {}

  void SetDate(int year, int month, int day) {
    year_ = year;
    month_ = month;
    day_ = day;
  }
  void BeginPass() {
#ifdef PADDLE_WITH_BOX_PS
    auto box_ptr = BoxWrapper::GetInstance();
    box_ptr->BeginPass();
#endif
  }
  void EndPass() {
#ifdef PADDLE_WITH_BOX_PS
    auto box_ptr = BoxWrapper::GetInstance();
    box_ptr->EndPass();
#endif
  }
  void LoadIntoMemory() {
    platform::Timer timer;
    VLOG(3) << "Begin LoadIntoMemory(), dataset[" << dataset_ << "]";
    timer.Start();
    dataset_->LoadIntoMemory();
    timer.Pause();
    VLOG(0) << "download + parse cost: " << timer.ElapsedSec() << "s";

    timer.Start();
    FeedPass();
    timer.Pause();
    VLOG(0) << "FeedPass cost: " << timer.ElapsedSec() << " s";
    VLOG(3) << "End LoadIntoMemory(), dataset[" << dataset_ << "]";
  }
  void PreLoadIntoMemory() {
    dataset_->PreLoadIntoMemory();
    feed_data_thread_.reset(new std::thread([&]() {
      dataset_->WaitPreLoadDone();
      FeedPass();
    }));
    VLOG(3) << "After PreLoadIntoMemory()";
  }
  void WaitFeedPassDone() { feed_data_thread_->join(); }

#ifdef PADDLE_WITH_BOX_PS
  // notify boxps to feed this pass feasigns from SSD to memory
  static void FeedPassThread(const std::deque<Record>& t, int begin_index,
                             int end_index, boxps::PSAgentBase* p_agent,
                             const std::unordered_set<int>& index_map,
                             int thread_id) {
    p_agent->AddKey(0ul, thread_id);
    for (auto iter = t.begin() + begin_index; iter != t.begin() + end_index;
         iter++) {
      const auto& ins = *iter;
      const auto& feasign_v = ins.uint64_feasigns_;
      for (const auto feasign : feasign_v) {
        if (index_map.find(feasign.slot()) != index_map.end()) {
          continue;
        }
        p_agent->AddKey(feasign.sign().uint64_feasign_, thread_id);
      }
    }
  }
#endif
  void FeedPass() {
    VLOG(3) << "Begin FeedPass";
#ifdef PADDLE_WITH_BOX_PS
    struct std::tm b;
    b.tm_year = year_ - 1900;
    b.tm_mon = month_ - 1;
    b.tm_mday = day_;
    b.tm_min = b.tm_hour = b.tm_sec = 0;
    std::time_t x = std::mktime(&b);

    auto box_ptr = BoxWrapper::GetInstance();
    auto input_channel_ =
        dynamic_cast<MultiSlotDataset*>(dataset_)->GetInputChannel();
    const std::deque<Record>& pass_data = input_channel_->GetData();

    // get feasigns that FeedPass doesn't need
    const std::unordered_set<std::string>& slot_name_omited_in_feedpass_ =
        box_ptr->GetOmitedSlot();
    std::unordered_set<int> slot_id_omited_in_feedpass_;
    const auto& all_readers = dataset_->GetReaders();
    PADDLE_ENFORCE_GT(all_readers.size(), 0,
                      platform::errors::PreconditionNotMet(
                          "Readers number must be greater than 0."));
    const auto& all_slots_name = all_readers[0]->GetAllSlotAlias();
    for (size_t i = 0; i < all_slots_name.size(); ++i) {
      if (slot_name_omited_in_feedpass_.find(all_slots_name[i]) !=
          slot_name_omited_in_feedpass_.end()) {
        slot_id_omited_in_feedpass_.insert(i);
      }
    }
    const size_t tnum = box_ptr->GetFeedpassThreadNum();
    boxps::PSAgentBase* p_agent = box_ptr->GetAgent();
    VLOG(3) << "Begin call BeginFeedPass in BoxPS";
    box_ptr->BeginFeedPass(x / 86400, &p_agent);

    std::vector<std::thread> threads;
    size_t len = pass_data.size();
    size_t len_per_thread = len / tnum;
    auto remain = len % tnum;
    size_t begin = 0;
    for (size_t i = 0; i < tnum; i++) {
      threads.push_back(
          std::thread(FeedPassThread, std::ref(pass_data), begin,
                      begin + len_per_thread + (i < remain ? 1 : 0), p_agent,
                      std::ref(slot_id_omited_in_feedpass_), i));
      begin += len_per_thread + (i < remain ? 1 : 0);
    }
    for (size_t i = 0; i < tnum; ++i) {
      threads[i].join();
    }
    VLOG(3) << "Begin call EndFeedPass in BoxPS";
    box_ptr->EndFeedPass(p_agent);
#endif
  }

 private:
  Dataset* dataset_;
  std::shared_ptr<std::thread> feed_data_thread_;
  int year_;
  int month_;
  int day_;
};

}  // end namespace framework
}  // end namespace paddle