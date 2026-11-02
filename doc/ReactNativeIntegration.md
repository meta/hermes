---
id: react-native-integration
title: React Native Integration
---

## Using a custom Hermes build in a React Native app

Since React Native 0.69, Hermes is shipped as part of the React Native distribution. We call this [bundled hermes](https://reactnative.dev/architecture/bundled-hermes).

If you wish to customize Hermes and use it inside your React Native app, you can do so by using the `REACT_NATIVE_OVERRIDE_HERMES_DIR` environment variable. This variable allows you to specify a folder where you can store your custom copy of the Hermes repository.

```bash
export REACT_NATIVE_OVERRIDE_HERMES_DIR=/path/to/your/hermes/repo
```

If you set this variable, React Native will use the Hermes build from the specified directory instead of the one that comes with the React Native distribution.

> [!NOTE]
> If you are making changes to the compiler in Hermes, be sure to make sure you test your app in release mode as this enables bytecode compilation in advance.

### Instructions for Android

On Android, once you set the `REACT_NATIVE_OVERRIDE_HERMES_DIR` environment variable, make sure you also:

1. Enable a build from source following the instructions in the [React Native website](https://reactnative.dev/contributing/how-to-build-from-source#update-your-project-to-build-from-source).
2. Re-run the Android app with the `yarn android` command.

### Instructions for iOS

On iOS, once you set the `REACT_NATIVE_OVERRIDE_HERMES_DIR` environment variable, make sure you also:

1. Disable React Native's Default Hermes Pod: Ensure that the `:hermes_enabled` flag is set to `false` in `ios/Podfile`.
```ruby
use_react_native!(
  :path => config[:reactNativePath],
  :hermes_enabled => false,
  # ... other configurations
)
```

2. Enable a build from source by installing the pods as follows:
```bash
BUILD_FROM_SOURCE=true bundle exec pod install
```
3. Re-run the iOS app with the `yarn ios` command.

## Reporting native crashes

If Hermes causes a native crash in your application, a stack trace is critical for us to be able to understand where the crash occurred. 

If you have a native crash to report, please be aware that in most cases, it is not feasible for us to debug crashes in earlier versions of Hermes. Please update your app to the latest React Native version, as that contains the latest hermes-engine versions. Specifically since React Native 0.69, Hermes is now coming [bundled together with React Native](https://reactnative.dev/architecture/bundled-hermes).

Including the symbolicated stack trace will make it easier for us to address your bug report more quickly.

### How to symbolicate a native crash on Android

To symbolicate a native crash on Android, you need to use the [`ndk-stack`](https://developer.android.com/ndk/guides/ndk-stack) command.
This command is part of the Android SDK and you should be able to access with `$ANDROID_HOME/ndk/<ndk_version>/ndk-stack` (provided you replace the URL with your NDK version like `$ANDROID_HOME/ndk/21.4.7075529/ndk-stack`).

To use the `ndk-stack` command you will need the debug symbols (check the paragraph below to see where to find them).

You can then symbolicate a stacktrace as follows:

```bash
$ANDROID_HOME/ndk/21.4.7075529/ndk-stack -sym ./<path-to-debug-symbols>/obj/local/arm64-v8a < crash.txt
```

provided that `crash.txt` contains your stacktrace in plain text, and you replace `arm64-v8a` with the correct architecture you used when the native crash was recorded.

As an alternative, you can trigger a crash and have a symbolicated logcat in real time with the following command:

```bash
adb logcat | $ANDROID_HOME/ndk/21.4.7075529/ndk-stack -sym ./<path-to-debug-symbols>/obj/local/arm64-v8a
```

### Where to find the debug symbols

#### React Native < 0.69

For React Native versions before 0.69 (i.e. hermes-engine 0.11.0 and previous versions), you can find the debug symbols in the [GitHub release](https://github.com/facebook/hermes/releases) corresponding to the Hermes NPM you used to build your application. 

Download the `hermes-runtime-android-vX.Y.Z.tar.gz` file for your version, unpack the tar file, then run `ndk-stack` using the contained directory as described in the paragraph above.

#### React Native 0.69 and 0.70

For React Native versions 0.69 and 0.70, you can find the debug symbols in the [GitHub release](https://github.com/facebook/react-native/releases) corresponding to the React Native version you used to build your application.

For instance for [React Native 0.70.6](https://github.com/facebook/react-native/releases/tag/v0.70.6), you can find the `hermes-native-symbols-v0.70.6.zip` file with the debug symbols inside.

#### React Native >= 0.71

For React Native versions 0.71 and above, the debug symbols are uploaded to Maven Central together with the `react-native` and `hermes-engine` artifacts. You don't need to manually download the debug symbols at all, as they will be available to you when you build your apps locally.

You can just invoke the `ndk-stack` command suggested above and the symbolication will just work.
