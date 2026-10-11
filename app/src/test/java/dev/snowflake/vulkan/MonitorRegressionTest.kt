package dev.snowflake.vulkan

import java.io.File
import org.junit.Assume.assumeTrue
import org.junit.Test

/**
 * Runs host regressions against production parsers without root or an Android device.
 */
class MonitorRegressionTest {
    /**
     * 验证呈现时间戳统计与环形缓冲区边界。
     */
    @Test fun presentedFrames() = FrameStatsTest.main(emptyArray())

    /**
     * 验证前台包名、用户与 Surface 的对应关系。
     */
    @Test fun foregroundIdentity() = ForegroundParsingTest.main(emptyArray())

    /**
     * 验证硬件计数器和功率单位转换。
     */
    @Test fun hardwareUnits() = HardwareUnitsTest.main(emptyArray())

    /**
     * 验证引擎报告的新鲜度与身份隔离。
     */
    @Test fun engineReports() = EngineResolutionTest.main(emptyArray())

    /**
     * 通过普通 shell 验证超时、输出上限和取消，不请求 root。
     */
    @Test fun boundedRootCommands() {
        assumeTrue(File("/bin/sh").canExecute())
        RootCommandTest.main(emptyArray())
    }
}
