/*
 * Copyright 2022-2024 Bull SAS
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <dlfcn.h>
#include <inttypes.h>
#include <math.h>
#include <mpi.h>
#include <omp-tools.h>
#include <omp.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <libgen.h> // for basename()

#include "moc.h"

#define CACHE_LINE 128

#define KMP_PAD(type, sz)                                                      \
    (sizeof(type) + (sz - ((sizeof(type) - 1) % (sz)) - 1))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

// macros for logs
#define log(format, ...)                                                       \
    do {printf("%s:%s:%d %s LOG " format, __FILE__, __func__, __LINE__, application_name, ##__VA_ARGS__); fflush(stdout);} while(0)
#define error(format, ...)                                                     \
    do {printf(                                                         \
           "%s:%s:%d %s ERROR " format, __FILE__, __func__, __LINE__, application_name, ##__VA_ARGS__); fflush(stdout);} while(0)
//#define __DEBUG
#ifdef __DEBUG
#define debug(format, ...)                                                     \
    do  {printf(                                                        \
                "%s:%s:%d %s DEBUG " format, __FILE__, __func__, __LINE__, application_name, ##__VA_ARGS__); fflush(stdout);} while(0)
#else
#define debug
#endif

static ompt_get_state_t ompt_get_state;
static ompt_get_proc_id_t ompt_get_proc_id;

static ompt_get_thread_data_t ompt_get_thread_data;
static ompt_get_unique_id_t ompt_get_unique_id;

int nb_cores = 48;
int is_init = 1;
int is_finalize = 0;
int nb_thread = 16;

int corepris[18];
struct stat stats;
char application_name[2048];

/**
 * shared data structure
 */
struct valeurfichier
{

    int lock;
    int nb_coeur;
    int coeur[48];
};

typedef struct
{
    double time; /* time zone */
    double time_cycle;
    double start;

    int nb_cycle;

    struct valeurfichier* ptr;
    int val_imposteur;

    int waitcore;
    int core_stable;

} callback_counter;

typedef union __attribute__((aligned(CACHE_LINE))) callback_counter_u
{
    double c_align; /* use worst case alignment */
    char c_pad[KMP_PAD(callback_counter, CACHE_LINE)];
    callback_counter cc;
} callback_counter_t;

static callback_counter_t* counter;

double ttime = 0;

int get_application_name(char *appname, int size) {
    char apppath[2048];
    memset(apppath, 0, sizeof(apppath));
    int rc = readlink("/proc/self/exe", apppath, sizeof(apppath) - 1);
    if (rc < 0) {
        perror("readlink failed");
        return 1;
    }
    char *appbasename = basename(apppath);
    strncpy(appname, appbasename, size);
    return 0;
}

/**
 * MPI_Finalize - know if the mpi_finalize is call
 * */
int
MPI_Finalize()
{
    is_finalize = 1;

    return PMPI_Finalize();
}

int
MPI_Init(int* a, char*** v)
{
    is_init = 1;

    return PMPI_Init(a, v);
}

/**
 * Called on imposter, by the 1st thread.
 * Reserves threads for all other imposter threads
 */
int
eat_all_core(int nb_core_need)
{
    int cell = 0;
    int nb_core = 0;
    char val_cell;
    int stop = 0;
    while (nb_core_need > nb_core && cell < nb_cores) {
        val_cell =
          __sync_val_compare_and_swap(&counter[0].cc.ptr->coeur[cell], 1, 0);

        if (val_cell == 1) {
            corepris[nb_core] = cell;
            nb_core++;
            debug("Je suis dans eat_all_core j'ai : %d core / %d core, j'ai "
                  "modifier la case %d elle vaut %d et je continue  \n",
                  nb_core,
                  nb_core_need,
                  cell,
                  counter[0].cc.ptr->coeur[cell]);
        }
        cell++;
    }
    if (cell == nb_cores && nb_core != nb_core_need) {
        for (int i = 0; i < nb_core; i++) {
            __sync_val_compare_and_swap(
              &counter[0].cc.ptr->coeur[corepris[i]], 0, 1);
            corepris[i] = -1;
        }
        debug("Je suis dans eat_all_core j'ai pas trouver les coeurs que je "
              "voulais \n");
    }

    return nb_core;
}

int
eat_one_core_in_all(int thread_id)
{
    int cell = 1;
    char val_cell;
    int find = 0;

    while (cell < nb_cores && find == 0) {
        val_cell =
          __sync_val_compare_and_swap(&counter[0].cc.ptr->coeur[cell], 1, 0);
        if (val_cell == 1) {
            find = 1;
            debug(
              "Je suis le thread %d. J'ai pris la case %c.", thread_id, cell);
        }
        cell++;
    }

    if (find == 1) {
        counter[thread_id].cc.waitcore = cell - 1;
    }
    return find;
}

/**
 * called on original app, by each application thread independently
 */
int
eat_one_core(int thread_id, int id_core)
{
    int cell_free = 0;
    int cell = id_core;
    char val;

    val = __sync_val_compare_and_swap(&counter[0].cc.ptr->coeur[cell], 1, -1);
    debug("Je suis dans eat_ONE_core je suis le thread : %d, j'ai lue/modifier "
          "la case %d elle vaut %d  \n",
          thread_id,
          cell,
          counter[0].cc.ptr->coeur[cell]);

    switch (val) {
        case (0):
            return 0;

        default:
            return 1;
    }
}

/**
 * give back the cores to state 1
 */
void
stop_eat_core(int id_core)
{

    if (counter[0].cc.val_imposteur == 1) {
        __sync_val_compare_and_swap(&counter[0].cc.ptr->coeur[id_core], 0, 1);
    } else {
        __sync_val_compare_and_swap(&counter[0].cc.ptr->coeur[id_core], -1, 1);
    }

    debug(
      "Je suis dans stop_eat_core, j'ai modifier la case %d elle vaut %d  \n",
      id_core,
      counter[0].cc.ptr->coeur[id_core]);
}

/**
 * wait for available thread
 * TODO: env variable ?
 * TODO: prioritize original app to regain threads
 */
void
would_eat()
{
    int tidd = omp_get_thread_num();
    debug(" c'est mon thread : %d, son coeur est pris il dort \n", tidd);
    usleep(1000);
    // usleep(3000);
}

/**
 * Imposter thread binding changed here.
 * Note: Intel specific code
 */
void
rebind_thread(int core)
{
    kmp_affinity_mask_t mask;
    kmp_create_affinity_mask(&mask);
    kmp_set_affinity_mask_proc(core, &mask);
    kmp_set_affinity(&mask);
    debug("Je change pour le %d core\n", core);
}

/**
 * This callback is left unused, as it has been found that it is not
 * allways called: e.g. not called in PATMOS application
 **/
static void
on_ompt_callback_work(ompt_work_t wstype,
                      ompt_scope_endpoint_t endpoint,
                      ompt_data_t* parallel_data,
                      ompt_data_t* task_data,
                      uint64_t count,
                      const void* codeptr_ra)
{}

/**
 * sync_region callback behaviour:
 *
 * for a pragma parallel
 *
 * 1st thread:
 * - parallel_begin
 * - sync_region(endpoint=scope_begin)
 * - sync_region(endpoint=scope_end)
 * - parallel_end
 *
 * other threads:
 * - [don't pass in parallel_begin]
 * - sync_region(endpoint=scope_end) [except 1st time]
 * - sync_region(endpoint=scope_begin)
 * - [don't pass in parallel_begin]
 *
 *
 * Only used for:
 * - kind: implicit barrier in omp parallel region (sync_region_barrier_implicit
 * / _implicit_parallel)
 * - state: ompt_state_overhead
 *
 *
 * Note:
 * - kind == ompt_sync_region_barrier_implicit_parallel (9) does not
 *   exist yet in intel compilers < 2021 (OMP 5.0)
 * - kind == ompt_sync_region_barrier_implicit (2) deprecated in intel
 *   compilers >= 2021 (OMP >= 5.1)
 * - Using a ifdef here below, because unfortunatly
 *   KMP_VERSION_MAJOR/MINOR says 5.0 in both intel versions :(
 */
#ifndef ompt_sync_region_barrier_implicit_parallel
#define ompt_sync_region_barrier_implicit_parallel 9
#endif
static void
on_ompt_callback_sync_region(ompt_sync_region_t kind,
                             ompt_scope_endpoint_t endpoint,
                             ompt_data_t* parallel_data,
                             ompt_data_t* task_data,
                             const void* codeptr_ra)
{
    // get info of thread and implicite region
    int tidd = omp_get_thread_num();
    int val;

    ompt_state_t state = ompt_get_state(NULL);
    int proc = ompt_get_proc_id();

    /**
     * Only for end of parallel region overhead
     */
    int is_fin = ((ompt_sync_region_barrier_implicit_parallel == kind ||
                   ompt_sync_region_barrier_implicit == kind) &&
                  state == ompt_state_overhead && is_finalize == 0);
    if (!is_fin) {
        return;
    }
    /**
     * 1st call (endpoint=begin)
     */
    if (ompt_scope_begin == endpoint) {

        if (counter[0].cc.val_imposteur == 1 &&
            tidd >= counter[0].cc.core_stable &&
            counter[tidd].cc.waitcore != -1) {
            //       printf("je veux liberer : %d\n",counter[tidd].cc.waitcore);
            stop_eat_core(counter[tidd].cc.waitcore);

        } else {
            if (counter[0].cc.val_imposteur != 1 && (tidd != 0) && tidd != 0 &&
                counter[0].cc.nb_cycle > 0) {
                // int core_free =  counter[0].cc.id_comm_world * nb_thread +
                // tidd;
                stop_eat_core(proc);
            }
        }

    } else if (ompt_scope_end == endpoint) {
        /**
         * threads other than 1 reserve a core
         */
        if (counter[0].cc.val_imposteur != 1 && tidd != 0) {

            while (!eat_one_core(tidd, proc) && is_finalize == 0) {
                would_eat();
            }
            // printf("dfghsgj\n");

        } else {
            /**
             * threads that have cores reservced rebind themselves
             */
            if (counter[0].cc.val_imposteur == 1 &&
                tidd >= counter[0].cc.core_stable &&
                counter[0].cc.nb_cycle > 1) {

                /*while(eat_one_core_in_all(tidd) == 0){
                  would_eat( );
                  }*/
                counter[tidd].cc.waitcore =
                  corepris[tidd - counter[0].cc.core_stable];
                rebind_thread(counter[tidd].cc.waitcore);

                // printf(" ---- %d ---- %d\n",tidd-counter[0].cc.core_stable,
                // counter[tidd].cc.waitcore);
            }
        }
    }
}

char* moc_file;

/**
 * We use the begin of parallel region:
 * - at the start to create the shared data structure
 * - the imposter reserves cores here
 */
static void
on_ompt_callback_parallel_begin(ompt_data_t* encountering_task_data,
                                const ompt_frame_t* encountering_task_frame,
                                ompt_data_t* parallel_data,
                                unsigned int requested_parallelism,
                                int flags,
                                const void* codeptr_ra)
{
    counter[0].cc.time_cycle = omp_get_wtime();
    ttime = omp_get_wtime();
    int nbth = omp_get_max_threads();
    debug("nbth=%d\n", nbth);
    for (int i = 0; i < 6; i++) {
        counter[i].cc.start = ttime;
    }

    /**
     * if it's the first cycle -> create the win shared
     */
    // if( is_init != 1 ){
    //  counter[0].cc.nb_cycle = -1;

    if (counter[0].cc.nb_cycle == 0) {
        moc_file = getenv("MOC_MAPFILE");

        if (NULL == moc_file)
            moc_file = "moc.dat";

        // cration du tableau
        int fd = open(moc_file, O_RDWR);

        fstat(fd, &stats);
        if (fd < 0) {
            error("\n could not open fichier\n");

            exit(1);
        }

        counter[0].cc.ptr =
          mmap(NULL, stats.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        if (counter[0].cc.ptr == MAP_FAILED) {
            error("Mapping Failed\n");
            exit(1);
        }

        close(fd);

        if (counter[0].cc.val_imposteur == 1) {
            int tidd;
            /**
             * Imposter's first thread gets a fixed core
             * as it does sequential work
             */
            counter[0].cc.core_stable = 1;

            for (tidd = 1; tidd < nbth; tidd++) {
                counter[tidd].cc.waitcore = -1;
            }
        }
    }

    /**
     * imposter tries to take cores
     */
    if (counter[0].cc.nb_cycle >= 1 && counter[0].cc.val_imposteur == 1) {

        debug("Je suis dans le parallel begin j'ai : %d core a avoir \n",
              nbth - counter[0].cc.core_stable);
        while (eat_all_core(nbth - counter[0].cc.core_stable) !=
               nbth - counter[0].cc.core_stable) {
            would_eat();
            debug("j'ai pas assez \n");
        }
    }

    counter[0].cc.nb_cycle++;
}

static void
on_ompt_callback_parallel_end(ompt_data_t* parallel_data,
                              ompt_data_t* task_data,
                              int flags,
                              const void* codeptr_ra)
{
    counter[0].cc.time_cycle = omp_get_wtime() - counter[0].cc.time_cycle;
    /* for( int i=0; i<48; i=i+1){
              printf( "%d | ", counter[0].cc.win_shared_other[i] );
          }
          printf( " ---- %d \n", counter[0].cc.id_comm_world );
    */
    /*if( counter[0].cc.val_imposteur == 1){
        printf("temps : %lf \n", counter[0].cc.time_cycle);
    }*/
}

/**
 * OMPT initialize and finalize
 **/
#define register_callback_t(name, type)                                        \
    do {                                                                       \
        type f_##name = &on_##name;                                            \
        if (ompt_set_callback(name, (ompt_callback_t)f_##name) ==              \
            ompt_set_never)                                                    \
            error("0: Could not register callback '" #name "'\n");             \
    } while (0)

#define register_callback(name) register_callback_t(name, name##_t)

int
ompt_initialize(ompt_function_lookup_t lookup,
                int initial_device_num,
                ompt_data_t* data)
{

    ompt_set_callback_t ompt_set_callback =
      (ompt_set_callback_t)lookup("ompt_set_callback");
    ompt_get_thread_data =
      (ompt_get_thread_data_t)lookup("ompt_get_thread_data");
    ompt_get_state = (ompt_get_state_t)lookup("ompt_get_state");
    ompt_get_proc_id = (ompt_get_proc_id_t)lookup("ompt_get_proc_id");
    ompt_get_unique_id = (ompt_get_unique_id_t)lookup("ompt_get_unique_id");
    //    ompt_callback_dispatch=(ompt_callback_dispatch_t)
    //    lookup("ompt_callback_dispatch");
    counter = malloc(sizeof(callback_counter_t) * nb_cores);
    for (int i = 0; i < nb_cores; i++) {
        counter[i].cc.time_cycle = 0;
        counter[i].cc.nb_cycle = 0;
    }

    counter[0].cc.nb_cycle = 0;

    char* imposteur = getenv("MOC_OPPORTUNIST");
    debug("MOC_OPPORTUNIST=%s\n", imposteur);
    counter[0].cc.val_imposteur = (imposteur!=NULL) ? atoi(imposteur) : 0;

    log("imposteur=%s appname=%s\n", imposteur, application_name);

    register_callback(ompt_callback_parallel_begin);
    register_callback(ompt_callback_parallel_end);
    register_callback(ompt_callback_sync_region);
    register_callback(ompt_callback_work);
    return 1; /*success*/
}

void
ompt_finalize(ompt_data_t* data)
{
    log("application runtime: %f    -----  \n",
        omp_get_wtime() - *(double*)(data->ptr));

    fflush(stdout);
}

ompt_start_tool_result_t*
ompt_start_tool(unsigned int omp_version, const char* runtime_version)
{
    get_application_name(application_name, 2048);
    debug("registering MOC tool\n");
    // static unsigned int periodic = 0;
    static double time = 0;
    time = omp_get_wtime();
    static ompt_start_tool_result_t ompt_start_tool_result = {
        &ompt_initialize, &ompt_finalize, { .ptr = &time }
    };
    return &ompt_start_tool_result;
}
