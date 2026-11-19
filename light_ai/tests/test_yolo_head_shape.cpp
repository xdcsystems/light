#include <sstream>

#include "check.hpp"
#include "core/yolo_decoder.hpp"
#include "suites.hpp"

void test_yolo_head_shape( TestRun& t )
{
    // Warns land in the sink; the checks look at the return value and the
    // out-params.
    std::ostringstream out;
    std::ostringstream err;
    const Log log( out, err );

    // Valid heads at every stride: stride and head derived from the map size.
    {
        const YoloHead h8 { nullptr, 255, 80, 80 };   // 640/80 = 8  -> P3
        const YoloHead h16 { nullptr, 255, 40, 40 };  // 640/40 = 16 -> P4
        const YoloHead h32 { nullptr, 255, 20, 20 };  // 640/20 = 32 -> P5
        int stride = 0;
        int head = -1;
        CHECK( t, yolo_head_shape( h8, 0, 640, 640, log, stride, head ) );
        CHECK( t, stride == 8 );
        CHECK( t, head == 0 );
        CHECK( t, yolo_head_shape( h16, 1, 640, 640, log, stride, head ) );
        CHECK( t, stride == 16 );
        CHECK( t, head == 1 );
        CHECK( t, yolo_head_shape( h32, 2, 640, 640, log, stride, head ) );
        CHECK( t, stride == 32 );
        CHECK( t, head == 2 );
    }
    // The width does not divide the model input.
    {
        const YoloHead h { nullptr, 255, 20, 39 };
        int stride = 0;
        int head = -1;
        CHECK( t, !yolo_head_shape( h, 0, 640, 640, log, stride, head ) );
        CHECK( t, stride == 0 );
        CHECK( t, head == -1 );
    }
    // The stride is not 8/16/32 (640/10 = 64).
    {
        const YoloHead h { nullptr, 255, 10, 10 };
        int stride = 0;
        int head = -1;
        CHECK( t, !yolo_head_shape( h, 1, 640, 640, log, stride, head ) );
    }
    // The height does not match the model at the stride (80 expected at 8).
    {
        const YoloHead h { nullptr, 255, 40, 80 };
        int stride = 0;
        int head = -1;
        CHECK( t, !yolo_head_shape( h, 2, 640, 640, log, stride, head ) );
    }
    // A non-3-anchor layout: ch 20 is per 6 with a remainder of 2.
    {
        const YoloHead h { nullptr, 20, 80, 80 };
        int stride = 0;
        int head = -1;
        CHECK( t, !yolo_head_shape( h, 0, 640, 640, log, stride, head ) );
    }
    // ch 90 is a valid 3-anchor layout (30 per anchor): the free function
    // accepts it -- the strict ch == 255 of the standard 80-class model is
    // the startup check's job.
    {
        const YoloHead h { nullptr, 90, 80, 80 };
        int stride = 0;
        int head = -1;
        CHECK( t, yolo_head_shape( h, 0, 640, 640, log, stride, head ) );
        CHECK( t, stride == 8 );
        CHECK( t, head == 0 );
    }
    // M-14: ch < 15 (per < 5) would let decode_anchor() read obj / box past
    // the head; ch 15 = 3 x (4 + 1) has no class channel; ch 18 (one class) is
    // the smallest usable head. A zero-height head (model height below the
    // stride) is rejected too.
    {
        int stride = 0;
        int head = -1;
        CHECK( t, !yolo_head_shape( YoloHead { nullptr, 15, 80, 80 }, 0, 640, 640, log, stride, head ) );
        CHECK( t, !yolo_head_shape( YoloHead { nullptr, 0, 80, 80 }, 0, 640, 640, log, stride, head ) );
        CHECK( t, !yolo_head_shape( YoloHead { nullptr, 3, 80, 80 }, 0, 640, 640, log, stride, head ) );
        CHECK( t, yolo_head_shape( YoloHead { nullptr, 18, 80, 80 }, 0, 640, 640, log, stride, head ) );
        CHECK( t, stride == 8 && head == 0 );
        CHECK( t, !yolo_head_shape( YoloHead { nullptr, 255, 0, 80 }, 0, 640, 4, log, stride, head ) );
    }
}
