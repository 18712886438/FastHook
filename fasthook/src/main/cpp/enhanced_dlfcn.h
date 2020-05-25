//
// Created on 2019/3/20.
//

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/system_properties.h>


bool file_exists(const char *name);

int enhanced_dlclose(void *handle);
void *enhanced_dlopen(const char *libpath, int flags);
void *enhanced_dlsym(void *handle, const char *name);
