# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

-keep class org.nopeforge.nopegl.NGLContext { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLContext { *; }
-keep class org.nopeforge.nopegl.NGLContext$Companion { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLContext$Companion { *; }
-keep class org.nopeforge.nopegl.NGLConfig { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLConfig { *; }
-keep class org.nopeforge.nopegl.NGLConfig$Companion { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLConfig$Companion { *; }
-keep class org.nopeforge.nopegl.NGLCustomTexture { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLCustomTexture { *; }
-keepclasseswithmembernames class org.nopeforge.nopegl.NGLCustomTexture$Callback { *; }
-keep class * extends org.nopeforge.nopegl.NGLCustomTexture$Callback { *; }
-keepclassmembers class * extends org.nopeforge.nopegl.NGLCustomTexture$Callback { *; }
-keepclassmembernames class * extends org.nopeforge.nopegl.NGLCustomTexture$Callback { *; }
-keep class org.nopeforge.nopegl.NGLError { *; }
-keep class org.nopeforge.nopegl.NGLError$Companion { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLError$Companion { *; }
-keep class org.nopeforge.nopegl.EGLHelper { *; }
-keepclassmembers class org.nopeforge.nopegl.EGLHelper { *; }
-keep class org.nopeforge.nopegl.NGLImageReader { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLImageReader { *; }
-keep class org.nopeforge.nopegl.NGLImageReader$Companion { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLImageReader$Companion { *; }
-keep class org.nopeforge.nopegl.NGLLogger { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLLogger { *; }
-keep class org.nopeforge.nopegl.NGLNode { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLNode { *; }
-keep class org.nopeforge.nopegl.NGLNode$Companion { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLNode$Companion { *; }
-keep class org.nopeforge.nopegl.NGLScene { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLScene { *; }
-keep class org.nopeforge.nopegl.NGLScene$Companion { *; }
-keepclassmembers class org.nopeforge.nopegl.NGLScene$Companion { *; }
-keep class org.nopeforge.nopemd.Player { *; }
-keepclassmembers class org.nopeforge.nopemd.Player { *; }
-keep class org.nopeforge.nopemd.Player$Companion { *; }
-keepclassmembers class org.nopeforge.nopemd.Player$Companion { *; }
