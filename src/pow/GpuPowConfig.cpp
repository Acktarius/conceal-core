// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license.

#include "GpuPowConfig.h"

#include "../Common/CommandLine.h"

namespace cn
{

namespace
{
// Name must not be a prefix of gpu-list / gpu-batch-size (boost parses --gpu-list as --gpu list).
const command_line::arg_descriptor<int> arg_gpu_device = {
    "gpu-device", "Enable CN-GPU PoW offload on OpenCL global device index (e.g. 0). Omit to disable.", -1};
const command_line::arg_descriptor<bool> arg_gpu_list = {
    "gpu-list", "List OpenCL platforms/GPU devices and exit", false};
const command_line::arg_descriptor<uint32_t> arg_gpu_batch_size = {
    "gpu-batch-size", "Max CN-GPU hashes per OpenCL batch", 128};
const command_line::arg_descriptor<uint32_t> arg_gpu_min_batch_size = {
    "gpu-min-batch-size",
    "Min jobs before GPU dispatch unless max-wait exceeded (0 = dispatch any partial batch)", 64};
const command_line::arg_descriptor<uint32_t> arg_gpu_max_wait_us = {
    "gpu-max-wait-us", "Max microseconds to wait for batch fill before flush", 9000};
const command_line::arg_descriptor<uint32_t> arg_gpu_prefetch_window = {
    "gpu-prefetch-window", "Max blocks ahead of tip to enqueue GPU PoW jobs", 1024};
const command_line::arg_descriptor<uint32_t> arg_gpu_backlog_threshold = {
    "gpu-backlog-threshold",
    "Min catch-up gap or batch size before GPU prefetch/consume (near-tip uses CPU only)", 16};
const command_line::arg_descriptor<uint32_t> arg_gpu_prefetch_depth = {
    "gpu-prefetch-depth", "Max GPU jobs queued/in-flight ahead of verify", 256};
const command_line::arg_descriptor<bool> arg_gpu_debug_crosscheck = {
    "gpu-debug-crosscheck", "Sample CPU re-verify of GPU PoW results", false};
const command_line::arg_descriptor<bool> arg_gpu_debug_inner = {
    "gpu-debug-inner", "Trace CN-GPU inner loop (1 iter) on CPU/GPU self-test failure", false};
const command_line::arg_descriptor<bool> arg_gpu_cpu_verify = {
    "gpu-cpu-verify",
    "Always recompute PoW on CPU at verify (disable GPU cache fast path)", false};
} // namespace

void GpuPowConfig::initOptions(boost::program_options::options_description& desc)
{
  command_line::add_arg(desc, arg_gpu_device);
  command_line::add_arg(desc, arg_gpu_list);
  command_line::add_arg(desc, arg_gpu_batch_size);
  command_line::add_arg(desc, arg_gpu_min_batch_size);
  command_line::add_arg(desc, arg_gpu_max_wait_us);
  command_line::add_arg(desc, arg_gpu_prefetch_window);
  command_line::add_arg(desc, arg_gpu_backlog_threshold);
  command_line::add_arg(desc, arg_gpu_prefetch_depth);
  command_line::add_arg(desc, arg_gpu_debug_crosscheck);
  command_line::add_arg(desc, arg_gpu_debug_inner);
  command_line::add_arg(desc, arg_gpu_cpu_verify);
}

void GpuPowConfig::init(const boost::program_options::variables_map& vm)
{
  deviceIndex = command_line::get_arg(vm, arg_gpu_device);
  listDevices = command_line::get_arg(vm, arg_gpu_list);
  batchSize = command_line::get_arg(vm, arg_gpu_batch_size);
  minBatchSize = command_line::get_arg(vm, arg_gpu_min_batch_size);
  maxWaitUs = command_line::get_arg(vm, arg_gpu_max_wait_us);
  if (minBatchSize > batchSize)
    minBatchSize = batchSize;
  prefetchWindow = command_line::get_arg(vm, arg_gpu_prefetch_window);
  backlogThreshold = command_line::get_arg(vm, arg_gpu_backlog_threshold);
  prefetchQueueDepth = command_line::get_arg(vm, arg_gpu_prefetch_depth);
  debugCrossCheck = command_line::get_arg(vm, arg_gpu_debug_crosscheck);
  debugInnerTrace = command_line::get_arg(vm, arg_gpu_debug_inner);
  trustGpuCache = !command_line::get_arg(vm, arg_gpu_cpu_verify);
}

} // namespace cn
