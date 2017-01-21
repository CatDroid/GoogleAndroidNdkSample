//
// Created by hanlon on 16-12-24.
//

#include "ExperimentalGradleAndroidPlugin.h"
#include <android/log.h>

ExperimentalGradleAndroidPlugin::ExperimentalGradleAndroidPlugin() {
    __android_log_print(ANDROID_LOG_WARN,"ExperimentalGradleAndroidPlugin" , "Constructor !" );
}

ExperimentalGradleAndroidPlugin::~ExperimentalGradleAndroidPlugin() {
    __android_log_print(ANDROID_LOG_WARN,"ExperimentalGradleAndroidPlugin" ,"Destructor");
}

void ExperimentalGradleAndroidPlugin::dumpInfo() {
    __android_log_print(ANDROID_LOG_WARN,"ExperimentalGradleAndroidPlugin","dumpInfo");
}