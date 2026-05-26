#pragma once

extern "C" {
    #include "postgres.h"
    #include "postmaster/bgworker.h"
}

void RegisterOtelWorker();