package pers.turing.technician.fasthook;

import android.os.Build;
import android.os.Handler;
import android.os.Message;
import android.util.Log;

import java.lang.reflect.Member;
import java.lang.StringBuilder;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;

public class FastHookManager {
    private static final String TAG = "FastHookManager";
    private static final boolean DEBUG = BuildConfig.DEBUG;

    private final static int ANDROID_P = 28;
    private final static int ANDROID_O_MR1 = 27;
    private final static int ANDROID_O = 26;
    private final static int ANDROID_N_MR1 = 25;
    private final static int ANDROID_N = 24;
    private final static int ANDROID_M = 23;
    private final static int ANDROID_L_MR1 = 22;
    private final static int ANDROID_L = 21;

    public final static int MODE_REWRITE = 1;
    public final static int MODE_REPLACE = 2;

    private final static int TYPE_RECORD_REWRITE = 1;
    private final static int TYPE_RECORD_REWRITE_HEAD = 2;
    private final static int TYPE_RECORD_REWRITE_TAIL = 3;
    private final static int TYPE_RECORD_REPLACE = 4;

    private final static int JIT_NONE = 0;
    private final static int JIT_COMPILE = 1;
    private final static int JIT_COMPILING = 2;
    private final static int JIT_COMPILINGORFAILED = 3;

    private final static int TYPE_TRAMPOLINE_JUMP = 0;
    private final static int TYPE_TRAMPOLINE_QUICK_HOOK = 1;
    private final static int TYPE_TRAMPOLINE_QUICK_ORIGINAL = 2;
    private final static int TYPE_TRAMPOLINE_QUICK_TARGET = 3;
    private final static int TYPE_TRAMPOLINE_HOOK = 4;
    private final static int TYPE_TRAMPOLINE_TARGET = 5;

    private final static int MESSAGE_RETRY = 1;

    private final static int RETRY_TIME_WAIT = 1000;
    private final static int RETRY_LIMIT = 1;

    private final static String HOOK_LIB = "fasthook";
    private final static String HOOK_ITEMS = "HOOK_ITEMS";
    private final static int HOOK_ITEM_SIZE = 10;
    private final static int HOOK_MODE_SIZE = 1;

    private final static String CONSTRUCTOR = "<init>";

    private static HashMap<Member,HookRecord> mHookMap;
    private static HashMap<Long,ArrayList<HookRecord>> mQuickTrampolineMap;
    private static Handler mHandler;

    static{
        System.loadLibrary(HOOK_LIB);
        mHookMap = new HashMap<Member,HookRecord>();
        mQuickTrampolineMap = new HashMap<Long,ArrayList<HookRecord>>();
        mHandler = new HookHandler();
        init(Build.VERSION.SDK_INT);
        if(Build.VERSION.SDK_INT >= ANDROID_P) {
            ReflectionUtils.passApiCheck();
        }
        Logd("Init");
    }

    public static void doHook(String hookInfoClassName, ClassLoader hookInfoClassLoader, ClassLoader targetClassLoader, ClassLoader hookClassLoader, ClassLoader forwardClassLoader, boolean jitInline) {
        try {
            String[][] hookItems = null;
            if(hookInfoClassLoader != null) {
                hookItems = (String[][])Class.forName(hookInfoClassName,true,hookClassLoader).getField(HOOK_ITEMS).get(null);
            }else {
                hookItems = (String[][])Class.forName(hookInfoClassName).getField(HOOK_ITEMS).get(null);
            }

            if(hookItems == null) {
                Loge("hook items is null");
                return;
            }

            for(int i = 0; i < hookItems.length; i++) {
                String[] hookItem = hookItems[i];
                if(hookItem.length != HOOK_ITEM_SIZE) {
                    Loge("invalid hook item size:"+hookItem.length+" item:"+ Arrays.toString(hookItem));
                    continue;
                }

                if(hookItem[0].length() != HOOK_MODE_SIZE) {
                    Loge("invalid hook mode size:"+hookItem[0].length()+" item:"+Arrays.toString(hookItem));
                    continue;
                }

                int mode = hookItem[0].charAt(0) - 48;
                if(mode > MODE_REPLACE || mode < MODE_REWRITE) {
                    mode = MODE_REWRITE;
                }

                Member targetMethod = getMethod(hookItem[1],hookItem[2],hookItem[3],targetClassLoader,null,null);
                Member hookMethod = getMethod(hookItem[4],hookItem[5],hookItem[6],null,hookClassLoader,null);
                Member forwardMethod = getMethod(hookItem[7],hookItem[8],hookItem[9],null,null,hookClassLoader);

                if(targetMethod == null || hookMethod == null) {
                    Loge("invalid target method or hook method item:"+Arrays.toString(hookItem));
                    continue;
                }

                if(forwardMethod != null && !isNativeMethod(forwardMethod)) {
                    Loge("forward method must be native method item:"+Arrays.toString(hookItem));
                    continue;
                }

                Logd("doHook Mode:"+mode+" TargetMethod["+hookItem[1]+","+hookItem[2]+","+hookItem[3]+"] HookMethod["+hookItem[4]+","+hookItem[5]+","+hookItem[6]+"] ForwardMethod["+hookItem[7]+","+hookItem[8]+","+hookItem[9]+"]");
                doHook(targetMethod,hookMethod,forwardMethod,mode,0);
            }

            if(!jitInline && Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                disableJITInline();
            }
        }catch (Exception e) {
            e.printStackTrace();
        }
    }

    private static void doHook(Member targetMethod, Member hookMethod, Member forwardMethod, int mode, int retryCount) {
        Logd("doHook target:"+targetMethod.getName()+" hook:"+hookMethod.getName()+" forward:"+((forwardMethod != null)?forwardMethod.getName():"null")+" model:"+mode+" retry:"+retryCount);

        HookRecord hookRecord = mHookMap.get(targetMethod);
        if(hookRecord != null) {
            Loge("already hook target:"+targetMethod.getName()+" hook:"+hookMethod.getName()+" forward:"+((forwardMethod != null)?forwardMethod.getName():"null")+" model:"+mode+" retry:"+retryCount);
            return;
        }

        boolean isNative = false;
        if(isNativeMethod(targetMethod)) {
            mode = MODE_REPLACE;
            isNative = true;
            Logd("do replace hook for native method");
        }

        if(mode == MODE_REPLACE && Build.VERSION.SDK_INT == ANDROID_L && !isNative) {
            mode = MODE_REWRITE;
        }

        if (mode == MODE_REPLACE) {
            if (Build.VERSION.SDK_INT < ANDROID_N) {
                doReplaceHookInternal(targetMethod, hookMethod, forwardMethod, isNative);
            } else {
                int jitState = checkJitState(targetMethod);
                Logd("jitState:" + jitState);

                switch (jitState) {
                    case JIT_COMPILING:
                    case JIT_COMPILINGORFAILED:
                        if (retryCount < RETRY_LIMIT) {
                            int newCount = retryCount + 1;
                            sendRetryMessage(targetMethod, hookMethod, forwardMethod, mode, newCount);
                            break;
                        }
                    case JIT_NONE:
                    case JIT_COMPILE:
                        if (Build.VERSION.SDK_INT >= ANDROID_O && BuildConfig.DEBUG) {
                            setNativeMethod(targetMethod);
                            Logd("set target method to native on debug mode");
                        }
                        doReplaceHookInternal(targetMethod, hookMethod, forwardMethod, isNative);
                        break;
                }
            }
        }

        Logd("doHook finish");
    }

    private static void doReplaceHookInternal(Member targetMethod, Member hookMethod, Member forwardMethod, boolean isNative) {
        HookRecord targetRecord = new HookRecord(TYPE_RECORD_REPLACE,targetMethod,hookMethod,forwardMethod,0,0);

        doReplaceHook(targetMethod,hookMethod,forwardMethod,isNative,targetRecord);
        Logd("QuickHookTrampoline:0x"+Long.toHexString(targetRecord.mHookTrampoline)+" QuickTargetTrampoline:0x"+Long.toHexString(targetRecord.mTargetTrampoline));

        mHookMap.put(targetMethod,targetRecord);
    }

    private static void sendRetryMessage(Member targetMethod, Member hookMethod, Member forwardMethod, int mode, int retryCount) {
        HookMessage hookMessage = new HookMessage(targetMethod, hookMethod, forwardMethod, mode, retryCount++);
        Message message = mHandler.obtainMessage(MESSAGE_RETRY);
        message.obj = hookMessage;

        mHandler.sendMessageDelayed(message, RETRY_TIME_WAIT);
    }

    public static Member getMethod(String className, String methodName, String paramSig, ClassLoader targetClassLoader, ClassLoader hookClassLoader, ClassLoader forwardClassLoader) {
        if(className.isEmpty() || methodName.isEmpty()) {
            return null;
        }

        int index = 0;
        ArrayList<Class> paramsArray = new ArrayList<Class>();
        while(index < paramSig.length()) {
            switch (paramSig.charAt(index)) {
                case 'B': // byte
                    paramsArray.add(byte.class);
                    break;
                case 'C': // char
                    paramsArray.add(char.class);
                    break;
                case 'D': // double
                    paramsArray.add(double.class);
                    break;
                case 'F': // float
                    paramsArray.add(float.class);
                    break;
                case 'I': // int
                    paramsArray.add(int.class);
                    break;
                case 'J': // long
                    paramsArray.add(long.class);
                    break;
                case 'S': // short
                    paramsArray.add(short.class);
                    break;
                case 'Z': // boolean
                    paramsArray.add(boolean.class);
                    break;
                case 'L':
                    try {
                        String objectClass = getObjectClass(index,paramSig);

                        if(hookClassLoader != null) {
                            paramsArray.add(Class.forName(objectClass,true,hookClassLoader));
                        }else if(forwardClassLoader != null) {
                            paramsArray.add(Class.forName(objectClass,true,forwardClassLoader));
                        }else if(targetClassLoader != null) {
                            paramsArray.add(Class.forName(objectClass,true,targetClassLoader));
                        }else {
                            paramsArray.add(Class.forName(objectClass));
                        }

                        index += objectClass.length() + 1;
                    }catch (Exception e) {
                        e.printStackTrace();
                    }
                    break;
                case '[':
                    try {
                        String arrayClass = getArrayClass(index,paramSig);

                        if(hookClassLoader != null) {
                            paramsArray.add(Class.forName(arrayClass,true,hookClassLoader));
                        }else if(forwardClassLoader != null) {
                            paramsArray.add(Class.forName(arrayClass,true,forwardClassLoader));
                        }else if(targetClassLoader != null) {
                            paramsArray.add(Class.forName(arrayClass,true,targetClassLoader));
                        }else {
                            paramsArray.add(Class.forName(arrayClass));
                        }

                        index += arrayClass.length() - 1;
                    }catch (Exception e) {
                        e.printStackTrace();
                    }
                    break;
            }
            index++;
        }

        Class[] params = new Class[paramsArray.size()];
        for(int i = 0; i < paramsArray.size(); i++) {
            params[i] = paramsArray.get(i);
        }

        Member method = null;
        try {
            if(hookClassLoader != null) {
                if(CONSTRUCTOR.equals(methodName)) {
                    method = Class.forName(className,true,hookClassLoader).getDeclaredConstructor(params);
                }else {
                    method = Class.forName(className,true,hookClassLoader).getDeclaredMethod(methodName,params);
                }
            }else if(forwardClassLoader != null) {
                if(CONSTRUCTOR.equals(methodName)) {
                    method = Class.forName(className,true,forwardClassLoader).getDeclaredConstructor(params);
                }else {
                    method = Class.forName(className,true,forwardClassLoader).getDeclaredMethod(methodName,params);
                }
            }else if(targetClassLoader != null) {
                if(CONSTRUCTOR.equals(methodName)) {
                    method = Class.forName(className,true,targetClassLoader).getDeclaredConstructor(params);
                }else {
                    method = Class.forName(className,true,targetClassLoader).getDeclaredMethod(methodName,params);
                }
            }else {
                if(CONSTRUCTOR.equals(methodName)) {
                    method = Class.forName(className).getDeclaredConstructor(params);
                }else {
                    method = Class.forName(className).getDeclaredMethod(methodName,params);
                }
            }
        }catch (Exception e) {
            e.printStackTrace();
        }

        return method;
    }

    private static String getObjectClass(int index, String paramSig) {
        String objectClass = null;

        String subParam = paramSig.substring(index + 1);
        objectClass = subParam.split(";")[0].replace('/','.');

        return objectClass;
    }

    private static String getArrayClass(int index, String paramSig) {
        int count = 0;
        StringBuilder arrayClassBuilder = new StringBuilder("");

        while(paramSig.charAt(index) == '[') {
            count++;
            index++;
            arrayClassBuilder.append('[');
        }

        if(paramSig.charAt(index) == 'L') {
            String subParam = paramSig.substring(index);
            arrayClassBuilder.append(subParam.split(";")[0].replace('/','.'));
            arrayClassBuilder.append(";");
        }else {
            arrayClassBuilder.append(paramSig.charAt(index));
        }

        return arrayClassBuilder.toString();
    }

    private static class HookRecord {
        public int mType;
        public Member mTargetMethod;
        public Member mHookMethod;
        public Member mForwardMethod;
        public long mHookTrampoline;
        public long mTargetTrampoline;
        public int index;
        public ArrayList<HookRecord> mQuickTrampolineList;

        public HookRecord(int type, Member targetMethod, Member hookMethod, Member forwardMethod, long hookTrampoline, long targetTrampoline) {
            this.mType = type;
            this.mTargetMethod = targetMethod;
            this.mHookMethod = hookMethod;
            this.mForwardMethod = forwardMethod;
            this.mHookTrampoline = hookTrampoline;
            this.mTargetTrampoline = targetTrampoline;
        }

        public String toString() {
            StringBuilder sb = new StringBuilder("");
            return sb.toString();
        }
    }

    private static class HookHandler extends Handler {

        @Override
        public void handleMessage(Message msg) {
            switch (msg.what) {
                case MESSAGE_RETRY:
                    HookMessage hookMessage = (HookMessage)msg.obj;
                    doHook(hookMessage.mTargetMethod,hookMessage.mHookMethod,hookMessage.mForwardMethod,hookMessage.mMode,hookMessage.mRetryCount);
                    break;
            }
        }
    }

    private static class HookMessage {
        public Member mTargetMethod;
        public Member mHookMethod;
        public Member mForwardMethod;
        public int mMode;
        public int mRetryCount;

        HookMessage(Member targetMethod, Member hookMethod, Member forwardMethod, int mode, int retryCount) {
            this.mTargetMethod = targetMethod;
            this.mHookMethod = hookMethod;
            this.mForwardMethod = forwardMethod;
            this.mMode = mode;
            this.mRetryCount = retryCount;
        }
    }

    private static void Logd(String message) {
        if(DEBUG) {
            Log.d(TAG,message);
        }
    }

    private static void Loge(String message) {
        Log.d(TAG,message);
    }

    private native static int init(int version);
    private native static void disableJITInline();
    private native static long getMethodEntryPoint(Member method);
    private native static boolean compileMethod(Member method);
    private native static long getQuickToInterpreterBridge(Class targetClass, String methodName, String methodSig);
    private native static boolean isCompiled(Member method);
    private native static boolean doRewriteHookCheck(Member method);
    private native static boolean isNativeMethod(Member method);
    private native static void setNativeMethod(Member method);
    private native static int checkJitState(Member method);
    private native static int doReplaceHook(Member targetMethod, Member hookMethod, Member forwardMethod, boolean isNative, HookRecord targetRecord);
}