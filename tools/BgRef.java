/* BgRef.java — the phone's backdrop arithmetic, lifted verbatim out of
 * marimo-android's BgManager.java with the Android types replaced by plain ints
 * so it can run on a workstation JDK. It exists so the C port can be checked
 * against the real thing instead of against my reading of it:
 *
 *   javac -d /tmp/bgref tools/BgRef.java && java -cp /tmp/bgref BgRef
 *
 * must print the same two hashes as `./build/marimo --selftest` does on its
 * `bg:` lines. Same grid (java.util.Random, seed 0xC0FFEE), same floats, same
 * truncating /255 — if they ever disagree, the port is wrong.
 *
 * Not part of the build: nothing compiles this unless someone asks. */
import java.util.Random;

public class BgRef {
    static final int CELLS = 2;
    /* half of 480x640, with the phone's floors */
    static final int W = 240, H = 320;
    /* t = 0, so both offsets are zero */
    static final int LIGHT = 0xFFF89B2C, MID = 0xFFD25A1C, DARK = 0xFF813427;
    static final int SCRIM = 0x99000000;

    static float smooth(float t) { return t * t * (3f - 2f * t); }
    static float lerp(float a, float b, float t) { return a + (b - a) * smooth(t); }
    static int rgb(int r, int g, int b) { return 0xFF000000 | (r << 16) | (g << 8) | b; }

    static int blend(int base, int over, float t) {
        if (t <= 0f) return base;
        if (t >= 1f) return over;
        int ia = Math.round(255 * (1f - t));
        int a = 255 - ia;
        int r = (((over >> 16) & 0xFF) * a + ((base >> 16) & 0xFF) * ia) / 255;
        int g = (((over >> 8) & 0xFF) * a + ((base >> 8) & 0xFF) * ia) / 255;
        int b = ((over & 0xFF) * a + (base & 0xFF) * ia) / 255;
        return rgb(r, g, b);
    }

    /* blend(base, over) — over's alpha decides, as the scrim does */
    static int blendA(int base, int over) {
        int a = (over >>> 24) & 0xFF;
        if (a <= 0) return base;
        int ia = 255 - a;
        int r = (((over >> 16) & 0xFF) * a + ((base >> 16) & 0xFF) * ia) / 255;
        int g = (((over >> 8) & 0xFF) * a + ((base >> 8) & 0xFF) * ia) / 255;
        int b = ((over & 0xFF) * a + (base & 0xFF) * ia) / 255;
        return rgb(r, g, b);
    }

    static int triLerp(int light, int mid, int dark, float t) {
        if (t < 0.5f) return blend(light, mid, t * 2f);
        return blend(mid, dark, (t - 0.5f) * 2f);
    }

    static long fnvByte(long h, int b) { return (h ^ (b & 0xFF)) * 1099511628211L; }

    public static void main(String[] args) {
        int gw = CELLS + 2;
        int gh = Math.max(2, Math.round(CELLS * (float) H / W) + 2);
        float[][] grid = new float[gh][gw];
        Random rnd = new Random(0xC0FFEE);
        for (int gy = 0; gy < gh; gy++)
            for (int gx = 0; gx < gw; gx++)
                grid[gy][gx] = rnd.nextFloat() * 2f - 1f;

        float[][] field = new float[H][W];
        float gwx = gw, ghy = gh;
        for (int y = 0; y < H; y++) {
            float fy = (float) y / (H - 1) * ghy;
            int gy = (int) fy;
            float ty = fy - gy;
            int gy0 = gy % gh, gy1 = (gy + 1) % gh;
            for (int x = 0; x < W; x++) {
                float fx = (float) x / (W - 1) * gwx;
                int gx = (int) fx;
                float tx = fx - gx;
                int gx0 = gx % gw, gx1 = (gx + 1) % gw;
                float v00 = grid[gy0][gx0], v10 = grid[gy0][gx1];
                float v01 = grid[gy1][gx0], v11 = grid[gy1][gx1];
                float a = lerp(v00, v10, tx);
                float b = lerp(v01, v11, tx);
                field[y][x] = lerp(a, b, ty);
            }
        }

        long fh = 1469598103934665603L;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int bits = Float.floatToIntBits(field[y][x]);
                for (int k = 0; k < 4; k++) fh = fnvByte(fh, bits >> (8 * k));
            }

        long ph = 1469598103934665603L;
        for (int y = 0; y < H; y++) {
            float v = (float) y / (H - 1);
            float darkBias = 0.30f + 0.70f * v;
            for (int x = 0; x < W; x++) {
                float n = field[y][x];
                float t = (n * 0.5f + 0.5f);
                t = t * (1f + 0.7f * v) * darkBias;
                if (t < 0f) t = 0f;
                if (t > 1f) t = 1f;
                int px = blendA(triLerp(LIGHT, MID, DARK, t), SCRIM);
                for (int k = 0; k < 4; k++) ph = fnvByte(ph, px >> (8 * k));
            }
        }

        System.out.printf("java: field %dx%d grid %dx%d%n", W, H, gw, gh);
        System.out.printf("java: field hash %016x pixels(0xC0FFEE palette) %016x%n", fh, ph);
    }
}
