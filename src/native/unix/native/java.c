/*
   Copyright 2001-2004 The Apache Software Foundation.
 
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at
 
       http://www.apache.org/licenses/LICENSE-2.0
 
   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/
/* @version $Id: java.c,v 1.5 2005/01/04 15:59:59 jfclere Exp $ */
#include "jsvc.h"

#ifdef OS_CYGWIN
typedef long long __int64;
#endif
#include <jni.h>

#ifdef CHARSET_EBCDIC
#ifdef OSD_POSIX
#include <ascii_ebcdic.h>
#define jsvc_xlate_to_ascii(b) _e2a(b)
#define jsvc_xlate_from_ascii(b) _a2e(b)
#endif
#else
#define jsvc_xlate_to_ascii(b) /* NOOP */
#define jsvc_xlate_from_ascii(b) /* NOOP */
#endif

static JavaVM *jvm=NULL;
static JNIEnv *env=NULL;
static jclass cls=NULL;

#define FALSE 0
#define TRUE !FALSE

static void shutdown(JNIEnv *env, jobject source, jboolean reload) {
    log_debug("Shutdown requested (reload is %d)",reload);
    if (reload==TRUE) main_reload();
    else main_shutdown();
}
/* Automaticly restart when the JVM crashes */
static void java_abort123()
{
    exit(123);
}

char *java_library(arg_data *args, home_data *data) {
    char *libf=NULL;

    /* Did we find ANY virtual machine? */
    if (data->jnum==0) {
        log_error("Cannot find any VM in Java Home %s",data->path);
        return(false);
    }

    /* Select the VM */
    if (args->name==NULL) {
        libf=data->jvms[0]->libr;
        log_debug("Using default JVM in %s",libf);
    } else {
        int x;
        for (x=0; x<data->jnum; x++) {
            if (data->jvms[x]->name==NULL) continue;
            if (strcmp(args->name,data->jvms[x]->name)==0) {
                libf=data->jvms[x]->libr;
                log_debug("Using specific JVM in %s",libf);
                break;
            }
        }
        if (libf==NULL) {
            log_error("Invalid JVM name specified %s",args->name);
            return(NULL);
        }
    }
    return(libf);
}

/* Initialize the JVM and its environment, loading libraries and all */
bool java_init(arg_data *args, home_data *data) {
#ifdef OS_DARWIN
    dso_handle apph=NULL;
    char appf[1024];
    struct stat sb;
#endif /* ifdef OS_DARWIN */
    jint (*symb)(JavaVM **, JNIEnv **, JavaVMInitArgs *);
    JNINativeMethod nativemethod;
    JavaVMOption *opt=NULL;
    dso_handle libh=NULL;
    JavaVMInitArgs arg;
    char *libf=NULL;
    jint ret;
    int x;
    char loaderclass[]=LOADER;
    char shutdownclass[]="shutdown";
    char shutdownparams[]="(Z)V";

    /* Decide WHAT virtual machine we need to use */
    libf=java_library(args,data);
    if (libf==NULL) {
        log_error("Cannot locate JVM library file");
        return(false);
    }

    /* Initialize the DSO library */
    if (dso_init()!=true) {
        log_error("Cannot initialize the dynamic library loader");
        return(false);
    }

    /* Load the JVM library */
    libh=dso_link(libf);
    if (libh==NULL) {
        log_error("Cannot dynamically link to %s",libf);
        log_error("%s",dso_error());
        return(false);
    }
    log_debug("JVM library %s loaded",libf);

#ifdef OS_DARWIN
    /*
       MacOS/X actually has two libraries, one with the REAL vm, and one for
       the VM startup.
       before JVM 1.4.1 The first one (libappshell.dyld) contains CreateVM
       after JVM 1.4.1 The library name is libjvm_compat.dylib.
    */
    if (replace(appf,1024,"$JAVA_HOME/../Libraries/libappshell.dylib",
                 "$JAVA_HOME",data->path)!=0) {
        log_error("Cannot replace values in loader library");
        return(false);
    }
    if (stat(appf, &sb)) {
        if (replace(appf,1024,"$JAVA_HOME/../Libraries/libjvm_compat.dylib",
                    "$JAVA_HOME",data->path)!=0) {
            log_error("Cannot replace values in loader library");
            return(false);
        }
    }
    apph=dso_link(appf);
    if (apph==NULL) {
        log_error("Cannot load required shell library %s",appf);
        return(false);
    }
    log_debug("Shell library %s loaded",appf);
#endif /* ifdef OS_DARWIN */
    symb=dso_symbol(libh,"JNI_CreateJavaVM");
    if (symb==NULL) {
#ifdef OS_DARWIN
        symb=dso_symbol(apph,"JNI_CreateJavaVM");
        if (symb==NULL) {
#endif /* ifdef OS_DARWIN */
            log_error("Cannot find JVM library entry point");
            return(false);
#ifdef OS_DARWIN
        }
#endif /* ifdef OS_DARWIN */
    }
    log_debug("JVM library entry point found (0x%08X)",symb);

    /* Prepare the VM initialization arguments */
    arg.version=JNI_VERSION_1_2;
    arg.ignoreUnrecognized=FALSE;
    arg.nOptions=args->onum;
    arg.nOptions++; /* Add abort code */
    opt=(JavaVMOption *)malloc(arg.nOptions*sizeof(JavaVMOption));
    for (x=0; x<args->onum; x++) {
        opt[x].optionString=strdup(args->opts[x]);
        jsvc_xlate_to_ascii(opt[x].optionString);
        opt[x].extraInfo=NULL;
    }
    opt[x].optionString="abort";
    opt[x].extraInfo=java_abort123;
    arg.options=opt;

    /* Do some debugging */
    if (log_debug_flag==true) {
        log_debug("+-- DUMPING JAVA VM CREATION ARGUMENTS -----------------");
        log_debug("| Version:                       %x",arg.version);
        log_debug("| Ignore Unrecognized Arguments: %s",
                  arg.ignoreUnrecognized==TRUE?"True":"False");
        log_debug("| Extra options:                 %d",arg.nOptions);

        for (x=0; x<args->onum; x++) {
            jsvc_xlate_from_ascii(opt[x].optionString);
            log_debug("|   \"%s\" (0x%08x)",opt[x].optionString,
                      opt[x].extraInfo);
            jsvc_xlate_to_ascii(opt[x].optionString);
        }
        log_debug("+-------------------------------------------------------");
    }

    /* And finally create the Java VM */
    ret=(*symb)(&jvm, &env, &arg);
    if (ret!=0) {
        log_error("Cannot create Java VM");
        return(false);
    }
    log_debug("Java VM created successfully");

    jsvc_xlate_to_ascii(loaderclass);
    cls=(*env)->FindClass(env,loaderclass);
    jsvc_xlate_from_ascii(loaderclass);
    if (cls==NULL) {
        log_error("Cannot find daemon loader %s",loaderclass);
        return(false);
    }
    log_debug("Class %s found",loaderclass);

    jsvc_xlate_to_ascii(shutdownclass);
    nativemethod.name=shutdownclass;
    jsvc_xlate_to_ascii(shutdownparams);
    nativemethod.signature=shutdownparams;
    nativemethod.fnPtr=shutdown;
    if((*env)->RegisterNatives(env,cls,&nativemethod,1)!=0) {
        log_error("Cannot register native methods");
        return(false);
    }
    log_debug("Native methods registered");

    return(true);
}

/* Destroy the Java VM */
bool JVM_destroy(int exit) {
    jclass system=NULL;
    jmethodID method;
    char System[]="java/lang/System";
    char exitclass[]="exit";
    char exitparams[]="(I)V"; 

    jsvc_xlate_to_ascii(System); 
    system=(*env)->FindClass(env,System);
    jsvc_xlate_from_ascii(System);
    if (system==NULL) {
        log_error("Cannot find class %s",System);
        return(false);
    }

    jsvc_xlate_to_ascii(exitclass);
    jsvc_xlate_to_ascii(exitparams);
    method=(*env)->GetStaticMethodID(env,system,exitclass,exitparams);
    if (method==NULL) {
        log_error("Cannot found \"System.exit(int)\" entry point");
        return(false);
    }

    log_debug("Calling System.exit(%d)",exit);
    (*env)->CallStaticVoidMethod(env,system,method,(jint)exit);

    /* We shouldn't get here, but just in case... */
    log_debug("Destroying the Java VM");
    if ((*jvm)->DestroyJavaVM(jvm)!=0) return(false);
    log_debug("Java VM destroyed");
    return(true);
}

/* Call the load method in our DaemonLoader class */
bool java_load(arg_data *args) {
    jclass stringClass=NULL;
    jstring className=NULL;
    jstring currentArgument=NULL;
    jobjectArray stringArray=NULL;
    jmethodID method=NULL;
    jboolean ret=FALSE;
    int x;
    char lang[]="java/lang/String";
    char load[]="load";
    char loadparams[]="(Ljava/lang/String;[Ljava/lang/String;)Z";

    jsvc_xlate_to_ascii(args->clas);
    className=(*env)->NewStringUTF(env,args->clas);
    jsvc_xlate_from_ascii(args->clas);
    if (className==NULL) {
        log_error("Cannot create string for class name");
        return(false);
    }

    jsvc_xlate_to_ascii(lang);
    stringClass=(*env)->FindClass(env,lang);
    jsvc_xlate_from_ascii(lang);
    if (stringClass==NULL) {
        log_error("Cannot find class java/lang/String");
        return(false);
    }

    stringArray=(*env)->NewObjectArray(env,args->anum,stringClass,NULL);
    if (stringArray==NULL) {
        log_error("Cannot create arguments array");
        return(false);
    }

    for (x=0; x<args->anum; x++) {
        jsvc_xlate_to_ascii(args->args[x]);
        currentArgument=(*env)->NewStringUTF(env,args->args[x]);
        jsvc_xlate_from_ascii(args->args[x]);
        if (currentArgument==NULL) {
            log_error("Cannot create string for argument %s",args->args[x]);
            return(false);
        }
        (*env)->SetObjectArrayElement(env,stringArray,x,currentArgument);
    }

    jsvc_xlate_to_ascii(load);
    jsvc_xlate_to_ascii(loadparams);
    method=(*env)->GetStaticMethodID(env,cls,load,loadparams);
    if (method==NULL) {
        log_error("Cannot found Daemon Loader \"load\" entry point");
        return(false);
    }

    ret=(*env)->CallStaticBooleanMethod(env,cls,method,className,stringArray);
    if (ret==FALSE) {
        log_error("Cannot load daemon");
        return(false);
    }

    log_debug("Daemon loaded successfully");
    return(true);
}

/* Call the start method in our daemon loader */
bool java_start(void) {
    jmethodID method;
    jboolean ret;
    char start[]="start";
    char startparams[]="()Z";

    jsvc_xlate_to_ascii(start);
    jsvc_xlate_to_ascii(startparams); 
    method=(*env)->GetStaticMethodID(env,cls,start,startparams);
    if (method==NULL) {
        log_error("Cannot found Daemon Loader \"start\" entry point");
        return(false);
    }

    ret=(*env)->CallStaticBooleanMethod(env,cls,method);
    if (ret==FALSE) {
        log_error("Cannot start daemon");
        return(false);
    }

    log_debug("Daemon started successfully");
    return(true);
}

/* Call the stop method in our daemon loader */
bool java_stop(void) {
    jmethodID method;
    jboolean ret;
    char stop[]="stop";
    char stopparams[]="()Z";

    jsvc_xlate_to_ascii(stop);
    jsvc_xlate_to_ascii(stopparams); 
    method=(*env)->GetStaticMethodID(env,cls,stop,stopparams);
    if (method==NULL) {
        log_error("Cannot found Daemon Loader \"stop\" entry point");
        return(false);
    }

    ret=(*env)->CallStaticBooleanMethod(env,cls,method);
    if (ret==FALSE) {
        log_error("Cannot stop daemon");
        return(false);
    }

    log_debug("Daemon stopped successfully");
    return(true);
}

/* Call the version method in our daemon loader */
bool java_version(void) {
    jmethodID method;
    char version[]="version";
    char versionparams[]="()Z";

    jsvc_xlate_to_ascii(version);
    jsvc_xlate_to_ascii(versionparams); 
    method=(*env)->GetStaticMethodID(env,cls,version,versionparams);
    if (method==NULL) {
        log_error("Cannot found Daemon Loader \"version\" entry point");
        return(false);
    }

    (*env)->CallStaticVoidMethod(env,cls,method);
    return(true);
}

/* Call the check method in our DaemonLoader class */
bool java_check(arg_data *args) {
    jstring className=NULL;
    jmethodID method=NULL;
    jboolean ret=FALSE;
    char check[]="check";
    char checkparams[]="(Ljava/lang/String;)Z";

    log_debug("Checking daemon");

    jsvc_xlate_to_ascii(args->clas);
    className=(*env)->NewStringUTF(env,args->clas);
    jsvc_xlate_from_ascii(args->clas);
    if (className==NULL) {
        log_error("Cannot create string for class name");
        return(false);
    }

    jsvc_xlate_to_ascii(check);
    jsvc_xlate_to_ascii(checkparams);
    method=(*env)->GetStaticMethodID(env,cls,check,checkparams);
    if (method==NULL) {
        log_error("Cannot found Daemon Loader \"check\" entry point");
        return(false);
    }

    ret=(*env)->CallStaticBooleanMethod(env,cls,method,className);
    if (ret==FALSE) {
        log_error("An error was detected checking the %s daemon",args->clas);
        return(false);
    }

    log_debug("Daemon checked successfully");
    return(true);
}

/* Call the destroy method in our daemon loader */
bool java_destroy(void) {
    jmethodID method;
    jboolean ret;
    char destroy[]="destroy";
    char destroyparams[]="()Z";

    jsvc_xlate_to_ascii(destroy);
    jsvc_xlate_to_ascii(destroyparams); 
    method=(*env)->GetStaticMethodID(env,cls,destroy,destroyparams);
    if (method==NULL) {
        log_error("Cannot found Daemon Loader \"destroy\" entry point");
        return(false);
    }

    ret=(*env)->CallStaticBooleanMethod(env,cls,method);
    if (ret==FALSE) {
        log_error("Cannot destroy daemon");
        return(false);
    }

    log_debug("Daemon destroyed successfully");
    return(true);
}
