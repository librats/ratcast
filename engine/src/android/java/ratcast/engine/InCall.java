package ratcast.engine;

import android.content.Context;
import android.media.AudioAttributes;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.List;

/**
 * A call as the system is told of it, made and left by the engine's
 * enter_call() (engine/src/android/in_call.cpp; ratcast/engine/in_call.h):
 * the audio mode a call's, its sound routed to a headset or else the
 * loudspeaker as headsets come and go, audio focus held, and CallService
 * keeping the process in the foreground. Any thread.
 */
public final class InCall {
    private static final String TAG = "ratcast";

    /** Where a call's sound goes: the first of these there is. */
    private static final int[] ROUTES = {
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        AudioDeviceInfo.TYPE_BLE_HEADSET,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADSET,
        AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER,
    };

    private final Context context;
    private final AudioManager audio;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final int mode;
    private final AudioFocusRequest focus;
    private final AudioDeviceCallback devices = new AudioDeviceCallback() {
        @Override
        public void onAudioDevicesAdded(AudioDeviceInfo[] added) {
            route();
        }

        @Override
        public void onAudioDevicesRemoved(AudioDeviceInfo[] removed) {
            route();
        }
    };

    public InCall(Context context) {
        this.context = context.getApplicationContext();
        audio = this.context.getSystemService(AudioManager.class);
        mode = audio.getMode();
        audio.setMode(AudioManager.MODE_IN_COMMUNICATION);
        focus = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build())
                .setOnAudioFocusChangeListener(
                        change -> Log.i(TAG, "audio focus " + change), main)
                .build();
        audio.requestAudioFocus(focus);
        // Called at once with the devices there are, and as they change.
        audio.registerAudioDeviceCallback(devices, main);
        update();
    }

    /** CallService held for the call, or told again what it does. */
    public void update() {
        CallService.hold(context, CallService.CALL, 0);
    }

    @SuppressWarnings("deprecation")  // setSpeakerphoneOn(), before Android 12
    public void leave() {
        CallService.release(context, CallService.CALL);
        audio.unregisterAudioDeviceCallback(devices);
        if (Build.VERSION.SDK_INT >= 31) audio.clearCommunicationDevice();
        else audio.setSpeakerphoneOn(false);
        audio.abandonAudioFocusRequest(focus);
        audio.setMode(mode);
    }

    /** The call's sound to the first of ROUTES there is. */
    @SuppressWarnings("deprecation")  // setSpeakerphoneOn(), before Android 12
    private void route() {
        if (Build.VERSION.SDK_INT >= 31) {
            List<AudioDeviceInfo> there = audio.getAvailableCommunicationDevices();
            for (int type : ROUTES) {
                for (AudioDeviceInfo device : there) {
                    if (device.getType() != type) continue;
                    if (!device.equals(audio.getCommunicationDevice())) {
                        Log.i(TAG, "the call's sound to " + device.getProductName() + " ("
                                + type + ")");
                        audio.setCommunicationDevice(device);
                    }
                    return;
                }
            }
            return;
        }
        // Before Android 12: the loudspeaker unless something is plugged in
        // (Bluetooth's headsets would need SCO started, left to the system).
        boolean plugged = false;
        for (AudioDeviceInfo device : audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
            int type = device.getType();
            plugged |= type == AudioDeviceInfo.TYPE_WIRED_HEADSET
                    || type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES
                    || type == AudioDeviceInfo.TYPE_USB_HEADSET;
        }
        audio.setSpeakerphoneOn(!plugged);
    }
}
