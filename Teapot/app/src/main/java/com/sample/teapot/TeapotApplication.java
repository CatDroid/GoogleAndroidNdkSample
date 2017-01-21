/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.sample.teapot;

import javax.microedition.khronos.opengles.GL10;

import android.app.Application;
import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.NameNotFoundException;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.opengl.GLUtils;
import android.util.Log;
import android.widget.Toast;

import java.util.Arrays;

public class TeapotApplication extends Application {

    static final private String TAG = "TeapotApplication";
    public void onCreate(){
        super.onCreate();	
        Log.w(TAG, "Application onCreate");

        final PackageManager pm = getApplicationContext().getPackageManager();
        ApplicationInfo ai;
        try {
            ai = pm.getApplicationInfo( this.getPackageName(), 0);
        } catch (final NameNotFoundException e) {
            ai = null;
        }
        final String applicationName = (String) (ai != null ? pm.getApplicationLabel(ai) : "(unknown)");
        Toast.makeText(this, applicationName, Toast.LENGTH_SHORT).show();

        Log.w(TAG, "Application label is " + applicationName );
        Log.w(TAG, "PackageName is " +  this.getPackageName() );

        float[] mat4 = new float[16] ;
        float[] vec4 = new float[]{1.0f, 2.0f, 3.0f ,4.0f} ;
        android.opengl.Matrix.setIdentityM(mat4,0);
        Log.w(TAG, "mat = " + Arrays.toString(mat4));
        float[] ver4_result = new float[4] ;
        android.opengl.Matrix.multiplyMV(ver4_result,0 , mat4 , 0 ,vec4 , 0  );
        Log.w(TAG, "mat4_result = " + Arrays.toString(ver4_result));

    }
}
