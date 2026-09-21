#include "opendoor/app.h"

int main(int argc, char **argv) {
    OpendoorOptions options;
    int result = opendoor_parse_args(argc, argv, &options);
    if (result == 1) {
        return 0;
    }
    if (result != 0) {
        return result;
    }
    return opendoor_run(&options);
}
