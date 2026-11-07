#ifndef MPI_HELPERS_H
#define MPI_HELPERS_H

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef OPEN_MPI
#error "This project requires Open MPI."
#endif

// при ошибке mpi завершаем все процессы, чтобы остальные не зависли в ожидании
static inline void mpi_check(int error, const char *operation)
{
    if (error != MPI_SUCCESS) {
        char message[MPI_MAX_ERROR_STRING];
        int length = 0;
        MPI_Error_string(error, message, &length);
        fprintf(stderr, "%s: %.*s\n", operation, length, message);
        MPI_Abort(MPI_COMM_WORLD, error);
        abort();
    }
}

#define MPI_CALL(operation) mpi_check((operation), #operation)

// общая остановка при нарушении протокола или нехватке памяти
static inline void protocol_fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    MPI_Abort(MPI_COMM_WORLD, 1);
    abort();
}

#endif
