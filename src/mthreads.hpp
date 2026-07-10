#include <pthread.h>

int perftools_pthread_key_create(pthread_key_t *key,  
                                 void (*destr_function) (void *));
void *perftools_pthread_getspecific(pthread_key_t key);
int perftools_pthread_setspecific(pthread_key_t key, void *val);
int perftools_pthread_once(pthread_once_t *ctl,  
                           void  (*init_routine) (void));
