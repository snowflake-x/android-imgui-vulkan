package dev.snowflake.vulkan

/**
 * 在性能服务的单一工作线程上获取有明确数据来源的指标。
 *
 * 实现需要限制外部读取的超时和输出规模，并允许主线程通过 [close] 取消采样。
 */
interface MetricCollector : AutoCloseable {
    /**
     * 同步读取本轮指标；由服务的单一工作线程串行调用。
     *
     * @param config 本轮采样使用的配置快照。
     * @param previous 本轮之前的采集器结果，不包含上一轮残留值。
     * @return 稳定字段标识到显示值的映射；缺失指标应说明原因，不得伪造测量值。
     */
    fun sample(config: HudConfig, previous: Map<String, String>): Map<String, String>

    /** 取消正在运行的外部进程；允许主线程调用，不得等待采样完成。 */
    override fun close()
}
