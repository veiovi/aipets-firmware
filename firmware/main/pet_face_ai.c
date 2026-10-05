#include "pet_face_ai.h"

#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"

#define W 120
#define H 120

typedef enum {
    P_VOID, P_BACKDROP, P_GLOW, P_OUTLINE, P_METAL, P_METAL_LIGHT,
    P_METAL_MID, P_METAL_DARK, P_METAL_DEEP, P_EYE_WHITE, P_EYE_DARK,
    P_PINK, P_PINK_LIGHT, P_PINK_DARK, P_WARNING, P_DANGER, P_MUTED,
    P_COUNT,
} palette_index_t;

typedef struct {
    float eye_open;
    float pupil_scale;
    float mouth_open;
    float smile;
    float brow_tilt;
    float head_tilt;
    float cheek;
} pose_t;

typedef struct {
    lv_obj_t *canvas;
    uint16_t *pixels;
    uint16_t colors[P_COUNT];
    pose_t pose;
    uint32_t next_blink_ms;
    uint32_t blink_started_ms;
    uint32_t next_saccade_ms;
    int8_t gaze_x;
    int8_t gaze_y;
    float press;
    uint32_t frames;
    int64_t draw_total_us;
    int64_t draw_max_us;
} ai_scene_t;

static ai_scene_t s_ai;

static inline int clamp_i(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static inline float clamp_f(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

static inline void px(int x, int y, uint16_t color)
{
    if ((unsigned)x < W && (unsigned)y < H) s_ai.pixels[y * W + x] = color;
}

static void rect(int x, int y, int width, int height, uint16_t color)
{
    int x0 = clamp_i(x, 0, W);
    int y0 = clamp_i(y, 0, H);
    int x1 = clamp_i(x + width, 0, W);
    int y1 = clamp_i(y + height, 0, H);
    for (int yy = y0; yy < y1; ++yy) {
        for (int xx = x0; xx < x1; ++xx) s_ai.pixels[yy * W + xx] = color;
    }
}

static void circle(int cx, int cy, int radius, uint16_t color)
{
    int rr = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        int span = (int)sqrtf((float)(rr - y * y));
        rect(cx - span, cy + y, span * 2 + 1, 1, color);
    }
}

static void circle_band(int cx, int cy, int radius, int y0, int y1, uint16_t color)
{
    int rr = radius * radius;
    y0 = clamp_i(y0, cy - radius, cy + radius);
    y1 = clamp_i(y1, cy - radius, cy + radius);
    for (int y = y0; y < y1; ++y) {
        int dy = y - cy;
        int span = (int)sqrtf((float)(rr - dy * dy));
        rect(cx - span, y, span * 2 + 1, 1, color);
    }
}

static void ellipse(int cx, int cy, int rx, int ry, uint16_t color)
{
    if (rx <= 0 || ry <= 0) return;
    const int64_t rx2 = rx * rx;
    const int64_t ry2 = ry * ry;
    const int64_t limit = rx2 * ry2;
    for (int y = -ry; y <= ry; ++y) {
        int span = 0;
        while (span < rx && (int64_t)(span + 1) * (span + 1) * ry2 +
               (int64_t)y * y * rx2 <= limit) ++span;
        rect(cx - span, cy + y, span * 2 + 1, 1, color);
    }
}

static void line(int x0, int y0, int x1, int y1, int width, uint16_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        rect(x0 - width / 2, y0 - width / 2, width, width, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

typedef struct { int x; int y; } point_t;

static void polygon(const point_t *points, size_t count, int ox, int oy, uint16_t color)
{
    int min_y = H - 1, max_y = 0;
    for (size_t i = 0; i < count; ++i) {
        min_y = points[i].y < min_y ? points[i].y : min_y;
        max_y = points[i].y > max_y ? points[i].y : max_y;
    }
    min_y = clamp_i(min_y + oy, 0, H - 1);
    max_y = clamp_i(max_y + oy, 0, H - 1);
    for (int y = min_y; y <= max_y; ++y) {
        int nodes[16];
        size_t node_count = 0;
        for (size_t i = 0, j = count - 1; i < count; j = i++) {
            int yi = points[i].y + oy, yj = points[j].y + oy;
            if ((yi < y && yj >= y) || (yj < y && yi >= y)) {
                nodes[node_count++] = points[i].x + ox +
                    (y - yi) * (points[j].x - points[i].x) / (yj - yi);
            }
        }
        for (size_t i = 1; i < node_count; ++i) {
            int value = nodes[i];
            size_t j = i;
            while (j && nodes[j - 1] > value) { nodes[j] = nodes[j - 1]; --j; }
            nodes[j] = value;
        }
        for (size_t i = 0; i + 1 < node_count; i += 2) rect(nodes[i], y, nodes[i + 1] - nodes[i] + 1, 1, color);
    }
}

static float approach(float current, float target, float amount)
{
    return current + (target - current) * amount;
}

static pose_t target_pose(pet_face_state_t state, pet_expression_t expression,
                          float speech, float blink, uint32_t tick)
{
    pose_t p = { .eye_open = 1.0f, .pupil_scale = 1.0f, .mouth_open = speech,
                 .smile = 0.45f, .brow_tilt = 0, .head_tilt = 0, .cheek = 0.2f };
    switch (expression) {
        case PET_EXPRESSION_HAPPY: p.eye_open=.82f; p.smile=1; p.cheek=.92f; break;
        case PET_EXPRESSION_CURIOUS: p.eye_open=1.08f; p.smile=.28f; p.brow_tilt=.85f; p.head_tilt=-.8f; break;
        case PET_EXPRESSION_SURPRISED: p.eye_open=1.26f; p.pupil_scale=.78f; p.mouth_open=fmaxf(p.mouth_open,.78f); p.smile=0; break;
        case PET_EXPRESSION_SLEEPY: p.eye_open=.34f; p.smile=.3f; p.cheek=.08f; p.head_tilt=.28f; break;
        case PET_EXPRESSION_CONCERNED: p.eye_open=.86f; p.smile=-.45f; p.brow_tilt=-.85f; p.cheek=.05f; break;
        case PET_EXPRESSION_EXCITED: p.eye_open=1.18f; p.pupil_scale=1.12f; p.smile=1; p.cheek=1; p.mouth_open=fmaxf(p.mouth_open,.36f); break;
        case PET_EXPRESSION_SHY: p.eye_open=.72f; p.pupil_scale=.95f; p.smile=.62f; p.cheek=1; p.head_tilt=.44f; break;
        default: break;
    }
    if (state == PET_FACE_BOOTING) p.eye_open = .05f;
    else if (state == PET_FACE_LISTENING) { p.eye_open += .12f; p.pupil_scale += .06f; p.mouth_open = 0; }
    else if (state == PET_FACE_THINKING) { p.eye_open *= .9f; p.brow_tilt += .4f; p.head_tilt += sinf(tick * .0012f) * .35f; }
    else if (state == PET_FACE_OFFLINE) { p.eye_open *= .76f; p.smile=-.25f; p.brow_tilt=-.7f; }
    else if (state == PET_FACE_ERROR) { p.eye_open *= .82f; p.smile=-.58f; p.brow_tilt=-.95f; }
    p.eye_open = fmaxf(.04f, p.eye_open * (1.0f - blink * .98f));
    return p;
}

static void draw_backdrop(uint32_t tick)
{
    rect(0, 0, W, H, s_ai.colors[P_VOID]);
    circle(60, 60, 57, s_ai.colors[P_BACKDROP]);
    circle(60, 60, 53, s_ai.colors[P_GLOW]);
    circle(60, 62, 51, s_ai.colors[P_BACKDROP]);
    static const point_t stars[] = {{14,22},{104,30},{18,88},{100,91},{33,11},{91,13},{10,59},{110,60}};
    for (size_t i = 0; i < sizeof(stars) / sizeof(stars[0]); ++i) {
        if (((tick / 180) + i * 3) % 7 < 2) px(stars[i].x, stars[i].y, s_ai.colors[P_MUTED]);
    }
}

static void draw_antenna(int side, int wobble, int cx, int cy)
{
    int d = side < 0 ? -1 : 1;
    line(cx + d*32, cy-27, cx+d*(38+wobble), cy-41, 4, s_ai.colors[P_OUTLINE]);
    line(cx + d*32, cy-27, cx+d*(38+wobble), cy-41, 2, s_ai.colors[P_METAL_LIGHT]);
    circle(cx+d*(40+wobble), cy-45, 8, s_ai.colors[P_OUTLINE]);
    circle(cx+d*(40+wobble), cy-45, 6, s_ai.colors[P_METAL]);
    rect(cx+d*(40+wobble)-2, cy-49, 3, 3, s_ai.colors[P_EYE_WHITE]);
}

static void draw_mohawk(int cx, int cy, int flex)
{
    static const point_t outer[] = {{-12,-29},{-14,-54},{-9,-65},{-5,-54},{-2,-70},{3,-55},{7,-66},{10,-47},{14,-56},{14,-28}};
    static const point_t inner[] = {{-10,-29},{-11,-52},{-8,-61},{-5,-50},{-2,-66},{2,-52},{6,-62},{8,-44},{12,-52},{12,-29}};
    static const point_t shine[] = {{-7,-31},{-7,-51},{-5,-57},{-3,-47},{-1,-62},{2,-49},{5,-57},{5,-39},{8,-47},{8,-31}};
    polygon(outer, sizeof(outer)/sizeof(outer[0]), cx+flex, cy, s_ai.colors[P_OUTLINE]);
    polygon(inner, sizeof(inner)/sizeof(inner[0]), cx+flex, cy, s_ai.colors[P_PINK]);
    polygon(shine, sizeof(shine)/sizeof(shine[0]), cx+flex, cy, s_ai.colors[P_PINK_LIGHT]);
    rect(cx-10+flex,cy-45,3,13,s_ai.colors[P_PINK_DARK]);
    rect(cx+2+flex,cy-49,3,18,s_ai.colors[P_PINK_DARK]);
    rect(cx+9+flex,cy-42,3,11,s_ai.colors[P_PINK_DARK]);
}

static void draw_ear(int side, int cx, int cy)
{
    int d = side < 0 ? -1 : 1;
    int x = d * 43;
    point_t outer[]={{x-d*2,-10},{x+d*8,-5},{x+d*10,17},{x+d*5,25},{x-d*2,20}};
    point_t inner[]={{x,-7},{x+d*6,-3},{x+d*7,15},{x+d*4,21},{x,17}};
    polygon(outer,5,cx,cy,s_ai.colors[P_OUTLINE]); polygon(inner,5,cx,cy,s_ai.colors[P_METAL]);
    line(cx+x+d*2,cy, cx+x+d*5,cy+14,2,s_ai.colors[P_METAL_LIGHT]);
}

static void draw_bolt(int x, int y)
{
    circle(x,y,5,s_ai.colors[P_OUTLINE]); circle(x,y,4,s_ai.colors[P_METAL_MID]);
    rect(x-1,y-2,2,2,s_ai.colors[P_EYE_WHITE]); rect(x,y+1,2,2,s_ai.colors[P_METAL_DARK]);
}

static void draw_eye(int side, int cx, int cy)
{
    int x = cx + side * 19;
    int y = cy - 3;
    int height = clamp_i((int)lroundf(19 * fminf(s_ai.pose.eye_open, 1.15f)), 1, 22);
    int width = 15 + (s_ai.pose.eye_open > 1 ? 2 : 0);
    if (height <= 3) { line(x-width/2,y,x+width/2,y,3,s_ai.colors[P_OUTLINE]); return; }
    ellipse(x,y,width/2+2,height/2+2,s_ai.colors[P_OUTLINE]);
    ellipse(x,y,width/2,height/2,s_ai.colors[P_EYE_WHITE]);
    int pr = clamp_i((int)lroundf(5.2f*s_ai.pose.pupil_scale),3,6);
    int px0=x+s_ai.gaze_x, py0=y+s_ai.gaze_y;
    circle(px0,py0,pr+1,s_ai.colors[P_EYE_DARK]);
    rect(px0-2,py0-3,3,3,s_ai.colors[P_EYE_WHITE]);
    rect(px0+2,py0+3,2,2,s_ai.colors[P_METAL_LIGHT]);
}

static void draw_brows(int cx, int cy)
{
    int tilt=(int)lroundf(s_ai.pose.brow_tilt*3);
    line(cx-27,cy-19-tilt,cx-12,cy-19+tilt,3,s_ai.colors[P_OUTLINE]);
    line(cx+12,cy-19+tilt,cx+27,cy-19-tilt,3,s_ai.colors[P_OUTLINE]);
}

static void draw_mouth(int cx, int cy, uint32_t tick)
{
    float open=clamp_f(s_ai.pose.mouth_open,0,1), smile=clamp_f(s_ai.pose.smile,-1,1);
    int y=cy+20;
    if(open>.14f){
        int width=(int)lroundf(14+open*10+fmaxf(0,smile)*4);
        int height=(int)lroundf(4+open*11);
        if(((tick/90)%3)==0){ circle(cx,y+1,height/2+1,s_ai.colors[P_OUTLINE]); rect(cx-width/3,y-height/2,width*2/3,height,s_ai.colors[P_OUTLINE]); }
        else ellipse(cx,y,width/2,height/2,s_ai.colors[P_OUTLINE]);
        rect(cx-width/2+3,y+height/2-3,width-6,2,s_ai.colors[P_PINK_DARK]);
        if(open>.72f) rect(cx-3,y-height/2+2,6,2,s_ai.colors[P_EYE_WHITE]);
    } else if(smile>=.05f){
        int width=(int)lroundf(17+smile*6);
        line(cx-width/2,y-1,cx-width/4,y+3,3,s_ai.colors[P_OUTLINE]);
        line(cx-width/4,y+3,cx+width/4,y+3,3,s_ai.colors[P_OUTLINE]);
        line(cx+width/4,y+3,cx+width/2,y-1,3,s_ai.colors[P_OUTLINE]);
    } else {
        line(cx-9,y+3,cx-4,y,3,s_ai.colors[P_OUTLINE]); line(cx-4,y,cx+4,y,3,s_ai.colors[P_OUTLINE]); line(cx+4,y,cx+9,y+3,3,s_ai.colors[P_OUTLINE]);
    }
}

static void draw_effects(pet_face_state_t state, pet_expression_t expression,
                         int cx, int cy, uint32_t tick)
{
    if(state==PET_FACE_LISTENING || state==PET_FACE_CONNECTING){
        int pulse=(tick/130)%4;
        line(cx-52-pulse,cy-10,cx-52-pulse,cy+12,2,s_ai.colors[state==PET_FACE_LISTENING?P_PINK:P_METAL]);
        line(cx+52+pulse,cy-10,cx+52+pulse,cy+12,2,s_ai.colors[state==PET_FACE_LISTENING?P_PINK:P_METAL]);
        if(state==PET_FACE_LISTENING){ rect(cx-2,cy+35,4,7,s_ai.colors[P_PINK]); rect(cx-5,cy+41,10,2,s_ai.colors[P_PINK_LIGHT]); }
    } else if(state==PET_FACE_THINKING){
        for(int i=0;i<3;++i) circle(cx+25+i*6,cy-30+(int)((tick/120+i)%3)-1,2,i==1?s_ai.colors[P_PINK]:s_ai.colors[P_METAL_LIGHT]);
    } else if(state==PET_FACE_PROVISIONING){
        rect(cx+30,cy+30,12,8,s_ai.colors[P_METAL_LIGHT]); rect(cx+34,cy+25,4,7,s_ai.colors[P_METAL_LIGHT]);
    } else if(state==PET_FACE_OFFLINE){
        line(cx+30,cy+30,cx+42,cy+42,3,s_ai.colors[P_DANGER]); line(cx+29,cy+37,cx+34,cy+32,2,s_ai.colors[P_MUTED]); circle(cx+34,cy+38,2,s_ai.colors[P_MUTED]);
    } else if(state==PET_FACE_ERROR){
        circle(cx+35,cy+35,10,s_ai.colors[P_OUTLINE]); circle(cx+35,cy+35,8,s_ai.colors[P_DANGER]); rect(cx+34,cy+29,2,8,s_ai.colors[P_OUTLINE]); rect(cx+34,cy+39,2,2,s_ai.colors[P_OUTLINE]);
    }
    if(expression==PET_EXPRESSION_HAPPY || expression==PET_EXPRESSION_EXCITED){
        static const point_t stars[]={{-37,-24},{35,-22},{-40,20},{39,16}};
        for(size_t i=0;i<4;++i) if(((tick/100)+i)%2){ int x=cx+stars[i].x,y=cy+stars[i].y; rect(x-1,y-3,2,7,s_ai.colors[P_PINK_LIGHT]); rect(x-3,y-1,7,2,s_ai.colors[P_PINK_LIGHT]); }
    } else if(expression==PET_EXPRESSION_SURPRISED){ line(cx-38,cy-30,cx-43,cy-36,2,s_ai.colors[P_WARNING]); line(cx+38,cy-30,cx+43,cy-36,2,s_ai.colors[P_WARNING]); }
    else if(expression==PET_EXPRESSION_SHY){ rect(cx-34,cy+13,8,3,s_ai.colors[P_PINK]); rect(cx+26,cy+13,8,3,s_ai.colors[P_PINK]); }
    else if(expression==PET_EXPRESSION_SLEEPY){ int d=(tick/300)%4; rect(cx+31+d,cy-27-d,4,2,s_ai.colors[P_METAL_LIGHT]); rect(cx+36+d,cy-33-d,5,2,s_ai.colors[P_METAL_LIGHT]); }
}

esp_err_t pet_face_ai_create(lv_obj_t *parent, lv_obj_t **root_out)
{
    if (!parent || !root_out) return ESP_ERR_INVALID_ARG;
    memset(&s_ai, 0, sizeof(s_ai));
    static const uint32_t hex[P_COUNT]={0x08070c,0x18131f,0x272034,0x05070c,0x64afe7,0xa9dcff,0x438fc8,0x15517e,0x0b3156,0xf8fbff,0x05101d,0xf52daf,0xff82d8,0xa60773,0xffd665,0xff638c,0x6f7381};
    for(size_t i=0;i<P_COUNT;++i) s_ai.colors[i]=lv_color_to_u16(lv_color_hex(hex[i]));
    s_ai.pixels=heap_caps_aligned_alloc(64,W*H*sizeof(uint16_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_ai.pixels) s_ai.pixels=heap_caps_aligned_alloc(64,W*H*sizeof(uint16_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(!s_ai.pixels) return ESP_ERR_NO_MEM;
    lv_obj_t *root=lv_obj_create(parent); lv_obj_remove_style_all(root); lv_obj_set_size(root,360,360); lv_obj_center(root); lv_obj_remove_flag(root,LV_OBJ_FLAG_CLICKABLE);
    s_ai.canvas=lv_canvas_create(root); lv_canvas_set_buffer(s_ai.canvas,s_ai.pixels,W,H,LV_COLOR_FORMAT_RGB565);
    lv_image_set_scale(s_ai.canvas,768); lv_image_set_antialias(s_ai.canvas,false); lv_obj_center(s_ai.canvas); lv_obj_remove_flag(s_ai.canvas,LV_OBJ_FLAG_CLICKABLE);
    s_ai.pose=(pose_t){.eye_open=1,.pupil_scale=1,.smile=.45f,.cheek=.2f};
    s_ai.next_blink_ms=1800; s_ai.next_saccade_ms=900;
    *root_out=root;
    return ESP_OK;
}

void pet_face_ai_render(pet_face_state_t state, pet_expression_t expression,
                        uint8_t audio_level, uint32_t tick, bool pressed)
{
    if(!s_ai.canvas || !s_ai.pixels) return;
    int64_t started=esp_timer_get_time();
    float blink=0;
    if(!s_ai.blink_started_ms && tick>=s_ai.next_blink_ms) s_ai.blink_started_ms=tick;
    if(s_ai.blink_started_ms){ uint32_t e=tick-s_ai.blink_started_ms; if(e<90) blink=e/90.0f; else if(e<140) blink=1; else if(e<240) blink=1-(e-140)/100.0f; else {s_ai.blink_started_ms=0;s_ai.next_blink_ms=tick+2200+(esp_random()%4200);} }
    if(tick>=s_ai.next_saccade_ms){ s_ai.gaze_x=(int8_t)((int)(esp_random()%7)-3); s_ai.gaze_y=(int8_t)((int)(esp_random()%5)-2); s_ai.next_saccade_ms=tick+650+(esp_random()%1900); }
    if(state==PET_FACE_THINKING){ s_ai.gaze_x=((tick/450)%3-1)*3; s_ai.gaze_y=-2; }
    float speech=state==PET_FACE_SPEAKING ? audio_level/255.0f : 0;
    pose_t target=target_pose(state,expression,speech,blink,tick);
    float *current=(float*)&s_ai.pose,*desired=(float*)&target;
    for(size_t i=0;i<sizeof(pose_t)/sizeof(float);++i) current[i]=approach(current[i],desired[i],i==2?.38f:.2f);
    s_ai.press=approach(s_ai.press,pressed?1:0,pressed?.45f:.16f);
    draw_backdrop(tick);
    int cx=60+(int)lroundf(s_ai.pose.head_tilt*1.5f), cy=67+(int)lroundf(sinf(tick*.0021f)*.65f+s_ai.press*2);
    int wobble=(int)lroundf(sinf(tick*.0028f));
    draw_antenna(-1,-wobble,cx,cy); draw_antenna(1,wobble,cx,cy); draw_ear(-1,cx,cy); draw_ear(1,cx,cy);
    circle(cx,cy+2,44,s_ai.colors[P_OUTLINE]); circle(cx,cy+2,41,s_ai.colors[P_METAL]);
    circle_band(cx,cy+2,40,cy+21,cy+33,s_ai.colors[P_METAL_MID]);
    circle_band(cx,cy+2,40,cy+33,cy+43,s_ai.colors[P_METAL_DARK]);
    rect(cx-35,cy-31,8,54,s_ai.colors[P_METAL_LIGHT]); rect(cx+28,cy-25,6,48,s_ai.colors[P_METAL_LIGHT]);
    rect(cx-18,cy-36,36,4,s_ai.colors[P_METAL_LIGHT]);
    draw_mohawk(cx,cy,wobble); draw_bolt(cx-31,cy+28); draw_bolt(cx+31,cy+28); draw_bolt(cx,cy+39);
    draw_brows(cx,cy); draw_eye(-1,cx,cy); draw_eye(1,cx,cy);
    if(s_ai.pose.cheek>.35f){ rect(cx-35,cy+10,8,3,s_ai.colors[P_PINK]); rect(cx+27,cy+10,8,3,s_ai.colors[P_PINK]); }
    draw_mouth(cx,cy,tick); draw_effects(state,expression,cx,cy,tick);
    lv_obj_invalidate(s_ai.canvas);
    int64_t elapsed=esp_timer_get_time()-started; s_ai.draw_total_us+=elapsed; if(elapsed>s_ai.draw_max_us)s_ai.draw_max_us=elapsed;
    if(++s_ai.frames%300==0){ s_ai.draw_total_us=0;s_ai.draw_max_us=0; }
}
