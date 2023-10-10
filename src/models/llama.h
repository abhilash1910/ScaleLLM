#pragma once

#include <torch/torch.h>

#include "layers/activation.h"
#include "layers/attention.h"
#include "layers/embedding.h"
#include "layers/linear.h"
#include "layers/normalization.h"
#include "layers/pos_embedding.h"
#include "memory/kv_cache.h"
#include "models/args.h"
#include "models/input_parameters.h"

// port LLAMA's model to C++ API:
// https://github.com/facebookresearch/llama/blob/main/llama/model.py
namespace llm {
class LlamaFeedForwardImpl : public torch::nn::Module {
 public:
  LlamaFeedForwardImpl(const ModelArgs& args,
                       const QuantizationArgs& quant_args,
                       const ParallelArgs& parallel_args,
                       torch::ScalarType dtype,
                       const torch::Device& device) {
    act_with_mul_ = Activation::get_act_with_mul_func("silu", device);
    CHECK(act_with_mul_ != nullptr);

    const int64_t hidden_size = args.hidden_size();
    const int64_t intermediate_size = args.intermediate_size();

    // register the weight parameter
    w1_w3_ = register_module("w1_w3",
                             ColumnParallelLinear(hidden_size,
                                                  intermediate_size * 2,
                                                  /*bias=*/false,
                                                  /*gather_output=*/false,
                                                  quant_args,
                                                  parallel_args,
                                                  dtype,
                                                  device));
    w2_ = register_module("w2",
                          RowParallelLinear(intermediate_size,
                                            hidden_size,
                                            /*bias=*/false,
                                            /*input_is_parallelized=*/true,
                                            quant_args,
                                            parallel_args,
                                            dtype,
                                            device));
  }

  torch::Tensor forward(torch::Tensor x) {
    return w2_(act_with_mul_(w1_w3_(x)));
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    w1_w3_->load_state_dict(state_dict, {"w1.", "w3."});
    w2_->load_state_dict(state_dict.select("w2."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    w1_w3_->verify_loaded_weights(prefix + "[w1,w3].");
    w2_->verify_loaded_weights(prefix + "w2.");
  }

 private:
  // parameter members, must be registered
  ColumnParallelLinear w1_w3_{nullptr};
  RowParallelLinear w2_{nullptr};

  // calculate act(x) * y
  ActFunc act_with_mul_{nullptr};
};
TORCH_MODULE(LlamaFeedForward);

class LlamaAttentionImpl : public torch::nn::Module {
 public:
  LlamaAttentionImpl(const ModelArgs& args,
                     const QuantizationArgs& quant_args,
                     const ParallelArgs& parallel_args,
                     torch::ScalarType dtype,
                     const torch::Device& device)
      : parallel_args_(parallel_args) {
    const auto world_size = parallel_args.world_size();
    const int64_t hidden_size = args.hidden_size();
    const int64_t n_heads = args.n_heads();
    const int64_t n_kv_heads = args.n_kv_heads().value_or(n_heads);

    n_local_heads_ = n_heads / world_size;
    n_local_kv_heads_ = n_kv_heads / world_size;
    head_dim_ = hidden_size / n_heads;
    // size for q, k, v
    qkv_sizes_ = {n_local_heads_ * head_dim_,
                  n_local_kv_heads_ * head_dim_,
                  n_local_kv_heads_ * head_dim_};

    // register submodules
    wqkv_ = register_module(
        "wqkv",
        ColumnParallelLinear(hidden_size,
                             (n_heads + 2 * n_kv_heads) * head_dim_,
                             /*bias=*/false,
                             /*gather_output=*/false,
                             quant_args,
                             parallel_args,
                             dtype,
                             device));
    wo_ = register_module("wo",
                          RowParallelLinear(n_heads * head_dim_,
                                            hidden_size,
                                            /*bias=*/false,
                                            /*input_is_parallelized=*/true,
                                            quant_args,
                                            parallel_args,
                                            dtype,
                                            device));

    // initialize attention
    const float scale = 1.0f / std::sqrt(static_cast<float>(head_dim_));
    atten_ = register_module("atten",
                             AttentionWithRoPE(n_local_heads_,
                                               n_local_kv_heads_,
                                               head_dim_,
                                               scale,
                                               /*rotary_dim=*/head_dim_,
                                               args.rope_scaling(),
                                               args.rope_theta(),
                                               args.max_position_embeddings(),
                                               /*interleaved=*/true,
                                               dtype,
                                               device));
  }

  torch::Tensor forward(torch::Tensor x,
                        torch::Tensor positions,
                        KVCache& kv_cache,
                        const InputParameters& input_params) {
    const auto num_tokens = x.size(0);
    // (num_tokens, dim) x (dim, n_local_heads * head_dim)
    // => (num_tokens, n_local_heads * head_dim)
    auto qkv = wqkv_(x).split(/*split_size=*/qkv_sizes_, /*dim=*/-1);
    DCHECK_EQ(qkv.size(), 3);

    // calculate attention, output: (num_tokens, n_local_heads * head_dim)
    auto output =
        atten_(qkv[0], qkv[1], qkv[2], positions, kv_cache, input_params);
    return wo_(output);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    wqkv_->load_state_dict(state_dict, {"wq.", "wk.", "wv."});
    wo_->load_state_dict(state_dict.select("wo."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    wqkv_->verify_loaded_weights(prefix + "[wq,wk,wv].");
    wo_->verify_loaded_weights(prefix + "wo.");
  }

 private:
  // parameter members, must be registered
  ColumnParallelLinear wqkv_{nullptr};

  RowParallelLinear wo_{nullptr};

  // module members without parameters
  AttentionWithRoPE atten_{nullptr};

  ParallelArgs parallel_args_;

  // configs used in forward
  int64_t n_local_heads_;
  int64_t n_local_kv_heads_;
  int64_t head_dim_;

  // size for q, k, v
  std::vector<int64_t> qkv_sizes_;
};
TORCH_MODULE(LlamaAttention);

class LlamaTransformerBlockImpl : public torch::nn::Module {
 public:
  LlamaTransformerBlockImpl(const ModelArgs& args,
                            const QuantizationArgs& quant_args,
                            const ParallelArgs& parallel_args,
                            torch::ScalarType dtype,
                            const torch::Device& device)
      : parallel_args_(parallel_args) {
    // register submodules
    attention_ = register_module(
        "attention",
        LlamaAttention(args, quant_args, parallel_args, dtype, device));
    feed_forward_ = register_module(
        "feed_forward",
        LlamaFeedForward(args, quant_args, parallel_args, dtype, device));
    attention_norm_ = register_module(
        "attention_norm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));
    ffn_norm_ = register_module(
        "ffn_norm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));
  }

  torch::Tensor forward(torch::Tensor x,
                        torch::Tensor positions,
                        KVCache& kv_cache,
                        const InputParameters& input_params) {
    auto h =
        x + attention_(attention_norm_(x), positions, kv_cache, input_params);
    return h + feed_forward_(ffn_norm_(h));
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    // call each submodule's load_state_dict function
    attention_->load_state_dict(state_dict.select("attention."));
    feed_forward_->load_state_dict(state_dict.select("feed_forward."));
    attention_norm_->load_state_dict(state_dict.select("attention_norm."));
    ffn_norm_->load_state_dict(state_dict.select("ffn_norm."));
  }

  void verify_loaded_weights(const std::string& prefix) const {
    attention_->verify_loaded_weights(prefix + "attention.");
    feed_forward_->verify_loaded_weights(prefix + "feed_forward.");
    attention_norm_->verify_loaded_weights(prefix + "attention_norm.");
    ffn_norm_->verify_loaded_weights(prefix + "ffn_norm.");
  }

 private:
  // parameter members, must be registered
  LlamaAttention attention_{nullptr};

  LlamaFeedForward feed_forward_{nullptr};

  RMSNorm attention_norm_{nullptr};

  RMSNorm ffn_norm_{nullptr};

  ParallelArgs parallel_args_;
};
TORCH_MODULE(LlamaTransformerBlock);

class LlamaModelImpl : public torch::nn::Module {
 public:
  LlamaModelImpl(const ModelArgs& args,
                 const QuantizationArgs& quant_args,
                 const ParallelArgs& parallel_args,
                 torch::ScalarType dtype,
                 const torch::Device& device)
      : parallel_args_(parallel_args) {
    // register submodules
    tok_embeddings_ = register_module("tok_embeddings",
                                      ParallelEmbedding(args.vocab_size(),
                                                        args.hidden_size(),
                                                        parallel_args,
                                                        dtype,
                                                        device));
    blocks_ = register_module("layers", torch::nn::ModuleList());
    layers_.reserve(args.n_layers());
    for (int32_t i = 0; i < args.n_layers(); i++) {
      auto block =
          LlamaTransformerBlock(args, quant_args, parallel_args, dtype, device);
      layers_.push_back(block);
      blocks_->push_back(block);
    }
    norm_ = register_module(
        "norm",
        RMSNorm(args.hidden_size(), args.rms_norm_eps(), dtype, device));
    output_ = register_module("output",
                              ColumnParallelLinear(args.hidden_size(),
                                                   args.vocab_size(),
                                                   /*bias=*/false,
                                                   /*gather_output=*/true,
                                                   parallel_args,
                                                   dtype,
                                                   device));
  }

  // tokens: [num_tokens]
  // positions: [num_tokens] token pos in the sequence
  torch::Tensor forward(torch::Tensor tokens,
                        torch::Tensor positions,
                        std::vector<KVCache>& kv_caches,
                        const InputParameters& input_params) {
    auto h = tok_embeddings_(tokens);
    for (size_t i = 0; i < layers_.size(); i++) {
      auto& layer = layers_[i];
      h = layer(h, positions, kv_caches[i], input_params);
    }
    h = norm_(h);
    // select last token for each sequence
    h = h.index_select(/*dim=*/0, input_params.last_token_indicies);
    return output_(h);
  }

  // load the weight from the checkpoint
  void load_state_dict(const StateDict& state_dict) {
    tok_embeddings_->load_state_dict(state_dict.select("tok_embeddings."));
    // call each layer's load_state_dict function
    for (int i = 0; i < layers_.size(); i++) {
      layers_[i]->load_state_dict(
          state_dict.select("layers." + std::to_string(i) + "."));
    }
    norm_->load_state_dict(state_dict.select("norm."));
    output_->load_state_dict(state_dict.select("output."));
  }

  void verify_loaded_weights() const {
    tok_embeddings_->verify_loaded_weights("tok_embeddings.");
    for (int i = 0; i < layers_.size(); i++) {
      layers_[i]->verify_loaded_weights("layers." + std::to_string(i) + ".");
    }
    norm_->verify_loaded_weights("norm.");
    output_->verify_loaded_weights("output.");
  }

 private:
  // parameter members, must be registered
  ParallelEmbedding tok_embeddings_{nullptr};

  torch::nn::ModuleList blocks_{nullptr};
  // hold same data but different type as blocks_ to avoid type cast
  std::vector<LlamaTransformerBlock> layers_;

  RMSNorm norm_{nullptr};

  ColumnParallelLinear output_{nullptr};

  ParallelArgs parallel_args_;
};
TORCH_MODULE(LlamaModel);

}  // namespace llm