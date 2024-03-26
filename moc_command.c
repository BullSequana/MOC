/*
 * Copyright 2022-2024 Bull SAS
 */

#include <stdio.h>
#include <stdlib.h>
#include <omp.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
struct valeurfichier{

  int lock;
  int nombrecoeur;
  int coeur[48];

};

char *moc_file;

void moc_init() {
    struct valeurfichier test;
    int i;
    FILE* fichier = NULL;
    
    fichier = fopen(moc_file,"w");
    test.lock=-1;
    test.nombrecoeur=0;
    for(i=0;i<48;i++){
        test.coeur[i]=-1;
    }
    
    if (fichier != NULL)
    {
        // On peut lire et écrire dans le fichier
        fwrite(&test,sizeof(test),1,fichier);
        fputs("\n",fichier);	
    
        fclose(fichier);

    }
}

void moc_read() {
    struct valeurfichier *test2;
    int i;
    
    int fd = open(moc_file, O_RDWR);    
    test2 = mmap ( NULL, sizeof(struct valeurfichier),
                   PROT_READ | PROT_WRITE, MAP_SHARED , fd, 0 );
    printf("ilock : %d - nbcoeur : %d\n",test2->lock,test2->nombrecoeur);
    //test2->coeur[10]=4;
    for(i=0;i<48;i++){
        printf("%d - ",test2->coeur[i]);
    }


}

void moc_help() {
    printf("usage: moc init|read\n");
}

int main(int argc, char**argv) {
    if (argc != 2) {
        moc_help();
        return 1;
    } else if (0 == strcmp("init", argv[1])) {
        moc_file = getenv("MOC_MAPFILE");
        if (NULL == moc_file)
            moc_file = "moc.dat";

        printf("init file:%s\n", moc_file);
        moc_init();
        return 0;
    } else if (0 == strcmp("read", argv[1])) {
        printf("read file:%s\n", moc_file);
        moc_read();
        return 0;
    } else if (0 == strcmp("help", argv[1]) || 0 == strcmp("-h", argv[1])) {
        moc_help();
        return 0;
     } else {
        printf("moc: unknown command\n");
        moc_help();
        return 1;
    }
}
