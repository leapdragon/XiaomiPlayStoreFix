package xiaomiplaystorefix;

import android.content.Context;
import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Arrays;

/**
 * Keeps Play Store off the ROM's built-in Cronet (android.net.http.HttpEngine, backed by
 * libcronet in the Tethering APEX). Two changes are enough:
 *
 * 1. android.net.http.HttpEngine.Builder#build() throws, so no engine can be created from it.
 * 2. Play Store's bundled Cronet API wraps HttpEngine in
 *    org.chromium.net.impl.HttpEngineNativeProvider. Its boolean checks (isEnabled) report false
 *    and createBuilder() throws, so the provider is skipped and Play Store falls back to
 *    another network stack.
 *
 * Loaded into Play Store processes by the Zygisk library, which implements {@link #hook}.
 */
public final class Main {
    static final String TAG = "XiaomiPlayStoreFix";

    private static final String HTTP_ENGINE_BUILDER = "android.net.http.HttpEngine$Builder";
    private static final String NATIVE_PROVIDER = "org.chromium.net.impl.HttpEngineNativeProvider";

    private static boolean providerDone;

    /** Implemented natively on top of LSPlant. Returns the backup method, or null on failure. */
    static native Method hook(Member target, Object hooker, Method callback);

    private Main() {}

    /** Entry point, called from native code after the process is specialized. */
    public static void init() {
        try {
            Class<?> builder = Class.forName(HTTP_ENGINE_BUILDER);
            int n = 0;
            for (Method m : builder.getDeclaredMethods()) {
                if (m.getName().equals("build") && install(m, Fixed.fail(HTTP_ENGINE_BUILDER + "#build"))) n++;
            }
            Log.i(TAG, "Phase1: HttpEngine.Builder#build blocked (" + n + ")");
        } catch (ClassNotFoundException e) {
            Log.i(TAG, "Phase1: no android.net.http.HttpEngine on this device");
        }

        // The provider lives in Play Store's own class loader, which exists only once the
        // application is being created; hook that moment.
        try {
            Method newApplication = android.app.Instrumentation.class.getDeclaredMethod(
                    "newApplication", ClassLoader.class, String.class, Context.class);
            OnNewApplication h = new OnNewApplication();
            h.original = hook(newApplication, h, OnNewApplication.CALLBACK);
            if (h.original == null) {
                Log.e(TAG, "cannot hook Instrumentation#newApplication");
            } else {
                h.original.setAccessible(true);
            }
        } catch (Throwable t) {
            Log.e(TAG, "init failed", t);
        }
    }

    static synchronized void disableNativeProvider(ClassLoader appLoader) {
        if (providerDone) return;
        providerDone = true;
        Class<?> provider;
        try {
            provider = Class.forName(NATIVE_PROVIDER, false, appLoader);
        } catch (Throwable t) {
            Log.i(TAG, "Phase2: " + NATIVE_PROVIDER + " not present");
            return;
        }
        int checks = 0;
        int builders = 0;
        for (Method m : provider.getDeclaredMethods()) {
            String label = NATIVE_PROVIDER + "#" + m.getName();
            // Zero-argument booleans are the availability checks (isEnabled, possibly obfuscated);
            // this also leaves equals(Object) alone.
            if (m.getReturnType() == boolean.class && m.getParameterCount() == 0) {
                if (install(m, Fixed.value(false, label))) checks++;
            } else if (m.getName().equals("createBuilder")) {
                if (install(m, Fixed.fail(label))) builders++;
            }
        }
        Log.i(TAG, "Phase2: HttpEngineNativeProvider disabled (" + checks + " checks, "
                + builders + " builders)");
    }

    private static boolean install(Method m, Fixed replacement) {
        int mod = m.getModifiers();
        if (Modifier.isAbstract(mod) || Modifier.isNative(mod)) return false;
        try {
            return hook(m, replacement, Fixed.CALLBACK) != null;
        } catch (Throwable t) {
            Log.w(TAG, "cannot hook " + replacement.label, t);
            return false;
        }
    }

    private static Method callbackOf(Class<?> cls) {
        try {
            return cls.getDeclaredMethod("callback", Object[].class);
        } catch (NoSuchMethodException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    /**
     * Replacement body: return a fixed value or throw. Public, like its callback, because
     * LSPlant's generated stub calls it from another package.
     */
    public static final class Fixed {
        static final Method CALLBACK = callbackOf(Fixed.class);

        final String label;
        final Object result;
        final boolean fail;

        private Fixed(String label, Object result, boolean fail) {
            this.label = label;
            this.result = result;
            this.fail = fail;
        }

        static Fixed value(Object result, String label) {
            return new Fixed(label, result, false);
        }

        static Fixed fail(String label) {
            return new Fixed(label, null, true);
        }

        public Object callback(Object[] args) {
            if (fail) {
                Log.i(TAG, "refused " + label);
                throw new IllegalStateException("Built-in Cronet disabled by XiaomiPlayStoreFix");
            }
            return result;
        }
    }

    /** Wraps Instrumentation#newApplication(ClassLoader, String, Context). */
    public static final class OnNewApplication {
        static final Method CALLBACK = callbackOf(OnNewApplication.class);

        Method original;

        public Object callback(Object[] args) throws Throwable {
            disableNativeProvider((ClassLoader) args[1]);
            try {
                return original.invoke(args[0], Arrays.copyOfRange(args, 1, args.length));
            } catch (InvocationTargetException e) {
                throw e.getCause();
            }
        }
    }
}
