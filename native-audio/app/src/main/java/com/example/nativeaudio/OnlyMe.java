package com.example.nativeaudio;

/**
 * Created by hanlon on 16-12-24.
 */
public class OnlyMe {
    private static OnlyMe ourInstance = new OnlyMe();

    public static OnlyMe getInstance() {
        return ourInstance;
    }

    private OnlyMe() {
    }
}
