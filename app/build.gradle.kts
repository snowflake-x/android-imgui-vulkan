plugins { id("com.android.application") }
android {
    namespace = "dev.snowflake.vulkan"
    compileSdk = 35
    ndkVersion = "28.0.13004108"
    defaultConfig {
        applicationId = "dev.snowflake.vulkan"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"
        ndk { abiFilters += listOf("arm64-v8a", "x86_64") }
        externalNativeBuild { cmake { cppFlags += listOf("-std=c++17", "-Wall", "-Wextra", "-Wno-missing-field-initializers") } }
    }
    packaging { jniLibs { useLegacyPackaging = false } }
    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt"); version = "3.22.1" } }
    buildTypes { release { isMinifyEnabled = false } }
}
