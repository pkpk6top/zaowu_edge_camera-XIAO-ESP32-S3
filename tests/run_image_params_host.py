"""执行实际成像参数应用函数，核验不同传感器的增益单位。"""
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[1]
source = (repo / 'main/camera.c').read_text(encoding='utf-8')
start = source.index('static void apply_image_params(')
end = source.index('\nesp_err_t camera_init(', start)
production = source[start:end]
fields = ['awb', 'awb_gain', 'wb_mode', 'aec', 'aec2', 'ae_level', 'aec_value',
          'agc', 'agc_gain', 'gainceiling', 'brightness', 'contrast', 'saturation',
          'special_effect', 'lenc', 'bpc', 'wpc', 'raw_gma']
setters = ['whitebal', 'awb_gain', 'wb_mode', 'exposure_ctrl', 'aec2', 'ae_level',
           'gain_ctrl', 'gainceiling', 'aec_value', 'agc_gain', 'brightness',
           'contrast', 'saturation', 'special_effect', 'lenc', 'bpc', 'wpc', 'raw_gma']
stub = '''
#include <assert.h>
#include <stdio.h>
#define OV5640_PID 0x5640
#define ESP_LOGI(...) ((void)0)
typedef int gainceiling_t;
typedef struct { int FIELD_LIST; } cam_image_cfg_t;
typedef struct sensor sensor_t;
struct sensor { struct { int PID; } id; SETTER_LIST };
static int observed;
static int ignore(sensor_t *s, int v) { (void)s; (void)v; return 0; }
static int gain(sensor_t *s, int v) { (void)s; observed=v; return 0; }
'''.replace('FIELD_LIST', ','.join(fields)).replace(
    'SETTER_LIST', ''.join(f'int (*set_{s})(sensor_t *, int);' for s in setters))
main = '''
int main(void) {
    sensor_t sensor = {0};
    INIT_SETTERS
    sensor.set_gainceiling = gain;
    cam_image_cfg_t image = {.aec_value=-1, .agc_gain=-1};
    const int expected[] = {32,64,128,256,512,1023,1023};
    for (int level=0;level<=6;level++) {
        image.gainceiling = level;
        sensor.id.PID = OV5640_PID;
        apply_image_params(&sensor,&image);
        assert(observed == expected[level]);
        sensor.id.PID = 0x2642;
        apply_image_params(&sensor,&image);
        assert(observed == level);
    }
    puts("14 sensor gain ceiling scenarios passed");
}
'''.replace('INIT_SETTERS', ''.join(f'sensor.set_{s}=ignore;' for s in setters))
with tempfile.TemporaryDirectory(prefix='zaowu-image-params-') as temp:
    folder = Path(temp)
    c = folder / 'image_params.c'
    exe = folder / 'image_params.exe'
    c.write_text(stub + production + main, encoding='utf-8')
    subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
