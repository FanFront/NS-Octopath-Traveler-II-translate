plugins { id("com.android.application") }
val payloadAssets = providers.gradleProperty("s2aAssets")
val outputRoot = providers.gradleProperty("s2aBuildDir")
if (outputRoot.isPresent) layout.buildDirectory.set(file(outputRoot.get() + "/app"))
val licenseAssets = layout.buildDirectory.dir("generated/licenseAssets")
val copyNotices = tasks.register<Copy>("copyNotices") {
    from(rootProject.file("THIRD_PARTY_NOTICES.md"))
    into(licenseAssets)
    rename { "THIRD_PARTY_NOTICES.txt" }
}
tasks.named("preBuild").configure { dependsOn(copyNotices) }
android {
    sourceSets.getByName("main").assets.srcDir(licenseAssets.get().asFile)
    buildFeatures { resValues = true }
    namespace = "dev.ot2.port"
    compileSdk = 36
    ndkVersion = "28.2.13676358"
    defaultConfig {
        applicationId = providers.gradleProperty("s2aAppId").getOrElse("org.switch2android.host")
        minSdk = 33
        targetSdk = 36
        versionCode = providers.gradleProperty("s2aVersionCode").getOrElse("1").toInt()
        versionName = "0.1.0"
        resValue("string", "app_name", providers.gradleProperty("s2aName").getOrElse("Switch2Android"))
        testInstrumentationRunner = "dev.ot2.port.BundleImportInstrumentation"
        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild { cmake { arguments += "-DANDROID_STL=c++_static" } }
    }
    externalNativeBuild { cmake {
        path = file("../native/CMakeLists.txt"); version = "3.22.1"
        if (outputRoot.isPresent) buildStagingDirectory = file(outputRoot.get() + "/cxx")
    } }
    if (payloadAssets.isPresent) sourceSets.getByName("main").assets.srcDir(payloadAssets.get())
    androidResources { noCompress += "s2a" }
    buildTypes {
        getByName("release") {
            // Personal builds use the local Android debug signing key, but Release code
            // remains optimized and non-debuggable. Keep this key for upgrades.
            signingConfig = signingConfigs.getByName("debug")
            isDebuggable = false
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
