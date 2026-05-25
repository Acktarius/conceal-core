// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include <cstdio>
#include <cstring>

#include "../src/pow/mining/GpuMinerConfig.hpp"

using namespace cn;

static int testAlignTotalIntensity()
{
  if (GpuMinerConfig::alignTotalIntensity(1920) != 1920)
    return 1;
  if (GpuMinerConfig::alignTotalIntensity(1922) != 1920)
    return 2;
  if (GpuMinerConfig::alignTotalIntensity(100) != 96)
    return 3;
  if (GpuMinerConfig::alignTotalIntensity(20) != 0)
    return 4;
  return 0;
}

static int testParseValue()
{
  std::string addr;
  std::vector<GpuDeviceSpec> devices;
  std::string err;
  if (!GpuMinerConfig::parseValue("ccx1abc,0:1920,1:1152", addr, devices, err))
  {
    std::fprintf(stderr, "parse failed: %s\n", err.c_str());
    return 1;
  }
  if (addr != "ccx1abc" || devices.size() != 2)
    return 2;
  if (devices[0].deviceIndex != 0 || devices[0].alignedIntensity != 1920 ||
      devices[0].perThreadIntensity != 640)
    return 3;
  if (devices[1].perThreadIntensity != 384)
    return 4;
  return 0;
}

static int testExclusivity()
{
  GpuMinerConfig gpu;
  std::string err;
  GpuMinerConfig::parseValue("ccx1abc,0:1920", gpu.rewardAddress, gpu.devices, err);
  applyMiningModeExclusivity("ccx1cpu", gpu, [](const std::string&) {});
  if (gpu.enabled())
    return 1;
  return 0;
}

int main()
{
  int failures = 0;
  failures += testAlignTotalIntensity();
  failures += testParseValue();
  failures += testExclusivity();
  if (failures)
  {
    std::fprintf(stderr, "PowGpuMineEquivalenceTests: %d failure(s)\n", failures);
    return 1;
  }
  std::fprintf(stderr, "PowGpuMineEquivalenceTests: ok\n");
  return 0;
}
