package dev.snowflake.vulkan

import kotlin.math.abs

/**
 * Verifies device counter semantics and sensor units without Android services or root access.
 */
object HardwareUnitsTest {
    /**
     * Runs the parser regression checks and fails on invalid results.
     */
    @JvmStatic fun main(args: Array<String>) {
        val before = HardwareUnits.cpuTicks("cpu 100 20 30 400 10 5 6 7 80 2")!!
        val after = HardwareUnits.cpuTicks("cpu 120 20 40 470 10 5 6 7 90 2")!!
        check(abs(HardwareUnits.cpuLoad(before, after)!! - 30.0) < 0.000001) {
            "Guest ticks already included in user ticks must not be counted twice"
        }
        check(HardwareUnits.cpuLoad(before, before) == null) { "Unchanged counters are unavailable" }
        check(HardwareUnits.cpuLoad(after, before) == null) { "Reset counters must restart sampling" }
        check(HardwareUnits.cpuTicks("cpu0 1 2 3 4") == null) { "Only aggregate CPU counters are accepted" }
        check(HardwareUnits.cpuTicks("cpu 1 2 -3 4") == null) { "Negative CPU counters are invalid" }
        check(HardwareUnits.cpuTicks("cpu 1 2 bad 4") == null) { "Malformed CPU counters are invalid" }
        check(HardwareUnits.cpuTicks("cpu 1 2 3 4")!!.size == 8) { "Optional legacy CPU fields default to zero" }
        check(HardwareUnits.gpuBusy("500 1000") == 50.0) { "GPU load uses busy divided by total time" }
        check(HardwareUnits.gpuBusy("0 1000") == 0.0) { "A valid idle GPU window reports zero load" }
        check(HardwareUnits.gpuBusy("1 0") == null) { "A zero-length GPU accounting window is unavailable" }
        check(HardwareUnits.gpuBusy("1001 1000") == null) { "GPU busy time cannot exceed the total window" }
        check(HardwareUnits.gpuBusy("500 1000 extra") == null) { "Unknown GPU counter formats are rejected" }
        check(HardwareUnits.powerWatts("2500000", 0.000001) == 2.5) { "Microwatts convert to watts" }
        check(HardwareUnits.powerWatts("2500", 0.001) == 2.5) { "Milliwatts convert to watts" }
        check(HardwareUnits.powerWatts("-1", 1.0) == null) { "Negative CPU power is invalid" }
        check(HardwareUnits.powerWatts("NaN", 1.0) == null) { "Nonfinite sensor values are invalid" }
        check(HardwareUnits.powerWatts("1", Double.POSITIVE_INFINITY) == null) { "Nonfinite unit multipliers are invalid" }
        check(HardwareUnits.powerWatts("1e308", 10.0) == null) { "Power conversion overflow is unavailable" }
        println("HardwareUnitsTest: 18 checks passed")
    }
}
