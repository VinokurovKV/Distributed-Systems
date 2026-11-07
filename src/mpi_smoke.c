#include <mpi.h>
#include <stdio.h>
#include <string.h>

#ifndef OPEN_MPI
#error "This project requires Open MPI. Select its mpicc wrapper."
#endif

// проверяем библиотеку, число процессов и размещение по узлам
int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);

    int rank = 0;
    int size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    char library[MPI_MAX_LIBRARY_VERSION_STRING];
    int library_length = 0;
    MPI_Get_library_version(library, &library_length);

    if (strstr(library, "Open MPI") == NULL) {
        fprintf(stderr, "rank %d: the loaded library is not Open MPI\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
        return 1;
    }

    if (size != 8) {
        if (rank == 0) {
            fprintf(stderr, "Expected exactly 8 MPI processes, got %d\n", size);
        }
        MPI_Finalize();
        return 2;
    }

    char host[MPI_MAX_PROCESSOR_NAME];
    int host_length = 0;
    MPI_Get_processor_name(host, &host_length);
    printf("rank=%d size=%d host=%.*s\n", rank, size, host_length, host);

    if (rank == 0) {
        printf("MPI library: %.*s\n", library_length, library);
        printf("Environment check only; run merge for variant 54.\n");
    }

    MPI_Finalize();
    return 0;
}
