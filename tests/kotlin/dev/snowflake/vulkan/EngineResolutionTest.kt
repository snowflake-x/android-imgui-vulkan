package dev.snowflake.vulkan

import java.nio.channels.FileChannel
import java.nio.file.Files
import java.nio.file.StandardOpenOption
import java.util.Comparator

/**
 * Exercises actual-versus-output dimensions, freshness, and conservative engine identification.
 */
object EngineResolutionTest {
        /**
     * Runs the parser regression checks and fails on invalid results.
     */
    @JvmStatic
        fun main(args: Array<String>) {
            val game = "com.tencent.tmgp.sgame"
            val valid = """{"schemaVersion":1,"packageName":"$game","engine":"Unity","source":"URP14/pre-upscale","pid":1234,"elapsedRealtimeMs":10000,"renderWidth":1280,"renderHeight":720,"outputWidth":1920,"outputHeight":1080}"""
            val report = EngineTelemetry.parse(valid, game, 11000)
            check(report.width == 1280 && report.height == 720)
            check(report.outputWidth == 1920 && report.outputHeight == 1080)
            check(EngineTelemetry.parse(valid, game, 15000).width == 1280)
            rejects { EngineTelemetry.parse(valid, game, 15001) }
            rejects { EngineTelemetry.parse(valid, game, 9999) }
            rejects { EngineTelemetry.parse(valid, "com.tencent.tmgp.pubgmhd", 11000) }
            rejects { EngineTelemetry.parse(valid.replace("\"renderWidth\":1280", "\"renderWidth\":0"), game, 11000) }
            rejects { EngineTelemetry.parse(valid.replace("\"renderHeight\":720", "\"renderHeight\":32769"), game, 11000) }
            rejects { EngineTelemetry.parse(valid.replace("\"renderWidth\":1280", "\"renderWidth\":\"1280\""), game, 11000) }
            rejects { EngineTelemetry.parse(valid.replace("\"pid\":1234", "\"pid\":1.5"), game, 11000) }
            rejects { EngineTelemetry.parse(valid.replace("\"schemaVersion\":1", "\"schemaVersion\":2"), game, 11000) }
            rejects { EngineTelemetry.parse(valid.replace(",\"outputHeight\":1080", ""), game, 11000) }
            rejects { EngineTelemetry.parse(valid.replace("URP14/pre-upscale", "fake\\nvalue"), game, 11000) }
            rejects { EngineTelemetry.parse(valid + " trailing", game, 11000) }
            rejects { EngineTelemetry.parse(" ".repeat(4097), game, 11000) }
            rejects { EngineTelemetry.parse("{truncated", game, 11000) }
            val unity = EngineProcess.parseProbe(game, 10, "3456\n/libunity.so\nversionCode=1001\n")!!
            check(unity.engine == "Unity" && unity.androidUser == 10 && unity.versionCode == 1001L)
            check(EngineProcess.parseProbe(game, 0, "3456\n/libUnreal.so\n")!!.engine == "Unreal")
            check(EngineProcess.parseProbe(game, 0, "3456\n/libUE4.so\n")!!.engine == "Unreal")
            check(EngineProcess.parseProbe(game, 0, "3456\nunknown\nversionCode=10")!!.engine == "Unknown")
            check(EngineProcess.parseProbe(game, 0, "3456\n/libunity.so.fake\n") == null)
            check(EngineProcess.parseProbe(game, 0, "permission denied\n/libunity.so\n") == null)
            check(EngineProcess.parseProbe(game, 0, "-1\n/libunity.so\n") == null)
            check(EngineProcess.parseProbe(game, 0, "3456") == null)
            if (System.getProperty("os.name") == "Linux") checksActualMappedLibraryProbe(game)
            println("EngineResolutionTest: passed")
        }

        /**
         * Verifies the generated shell against a real mapped library and controlled package query.
         */
        private fun checksActualMappedLibraryProbe(game: String) {
            val directory = Files.createTempDirectory("vulkanscope-engine-probe")
            try {
                val library = directory.resolve("libunity.so")
                val options = arrayOf(StandardOpenOption.CREATE, StandardOpenOption.READ, StandardOpenOption.WRITE)
                FileChannel.open(library, *options).use { channel ->
                    val mapping = channel.map(FileChannel.MapMode.READ_WRITE, 0, 4096)
                    mapping.put(0, 42.toByte())
                    Files.write(directory.resolve("pidof"), "#!/bin/sh\nprintf '%s\\n' '${java.io.File("/proc/self").canonicalFile.name}'\n".toByteArray())
                    Files.write(directory.resolve("dumpsys"), "#!/bin/sh\nprintf '    versionCode=42 minSdk=26\\n'\n".toByteArray())
                    check(directory.resolve("pidof").toFile().setExecutable(true))
                    check(directory.resolve("dumpsys").toFile().setExecutable(true))
                    val uidCommand = ProcessBuilder("id", "-u").start()
                    val user = uidCommand.inputStream.bufferedReader().readText().trim().toInt() / 100000
                    check(uidCommand.waitFor() == 0)
                    val builder = ProcessBuilder("sh", "-c", EngineProcess.probeCommand(game, user))
                    builder.environment()["PATH"] = directory.toString() + ":" + builder.environment()["PATH"]
                    val process = builder.start()
                    val output = process.inputStream.bufferedReader().readText()
                    check(process.waitFor() == 0)
                    val result = EngineProcess.parseProbe(game, user, output)
                    check(result?.engine == "Unity" && result.versionCode == 42L) { output }
                    check(mapping.get(0) == 42.toByte())
                }
            } finally {
                Files.walk(directory).use { paths -> paths.sorted(Comparator.reverseOrder()).forEach { Files.deleteIfExists(it) } }
            }
        }

        private fun rejects(action: () -> Unit) {
            var rejected = false
            try { action() } catch (_: IllegalArgumentException) { rejected = true }
            check(rejected) { "Malformed or stale engine telemetry was accepted" }
        }
}
