// =========================================================
// extract_background — direct port of extract_background.py.
// Per-line correspondence preserved; comments mark each Python source line.
// =========================================================
#include "extract_background.hpp"
#include <opencv2/imgproc.hpp>

namespace fall {

ExtractBackground::ExtractBackground(int W, int H)
    : H_(H), W_(W),
      y_grid_(GRID_SIZE + 1),
      x_grid_(GRID_SIZE + 1),
      patch_states_(GRID_SIZE * GRID_SIZE, 0),
      patch_static_counter_(GRID_SIZE * GRID_SIZE, 0)
{
    // self.y_grid = np.linspace(0, H, self.GRID_SIZE + 1, dtype=int)
    // self.x_grid = np.linspace(0, W, self.GRID_SIZE + 1, dtype=int)
    // numpy.linspace with dtype=int truncates (floor for positive); we match.
    for (int i = 0; i <= GRID_SIZE; ++i) {
        // The Python expression evaluates as a float64 and is then cast to int
        // by numpy. For W=640,H=480,GRID_SIZE=16 the divisions are exact, but
        // we use the same `double` arithmetic + truncation just in case.
        y_grid_[i] = static_cast<int>(static_cast<double>(H_) * i / GRID_SIZE);
        x_grid_[i] = static_cast<int>(static_cast<double>(W_) * i / GRID_SIZE);
    }

    // patch_h = self.y_grid[1]-self.y_grid[0]
    // patch_w = self.x_grid[1]-self.x_grid[0]
    int patch_h = y_grid_[1] - y_grid_[0];
    int patch_w = x_grid_[1] - x_grid_[0];
    int patch_area = patch_w * patch_h;

    // self.moving_THRESHOLD = int(0.0075*patch_area)
    // if self.moving_THRESHOLD==0: self.moving_THRESHOLD=1
    moving_threshold_ = static_cast<int>(0.0075 * patch_area);
    if (moving_threshold_ == 0) moving_threshold_ = 1;
}

void ExtractBackground::reset() {
    init_ = false;
}

cv::Mat ExtractBackground::update_bg(const cv::Mat& current_frame) {
    // if not self.init: ...
    if (!init_) {
        // self.prev_gray = cv2.cvtColor(current_frame, cv2.COLOR_BGR2GRAY)
        cv::cvtColor(current_frame, prev_gray_, cv::COLOR_BGR2GRAY);
        // self.static_frame = current_frame.copy()
        static_frame_ = current_frame.clone();
        // self.init = True
        init_ = true;
        // return current_frame
        return current_frame;
    }

    // gray = cv2.cvtColor(current_frame, cv2.COLOR_BGR2GRAY)
    cv::Mat gray;
    cv::cvtColor(current_frame, gray, cv::COLOR_BGR2GRAY);
    // diff = cv2.absdiff(gray, self.prev_gray)
    cv::Mat diff;
    cv::absdiff(gray, prev_gray_, diff);

    // for i in range(self.GRID_SIZE):
    //     for j in range(self.GRID_SIZE):
    for (int i = 0; i < GRID_SIZE; ++i) {
        for (int j = 0; j < GRID_SIZE; ++j) {
            // y1, y2 = self.y_grid[i], self.y_grid[i+1]
            // x1, x2 = self.x_grid[j], self.x_grid[j+1]
            int y1 = y_grid_[i], y2 = y_grid_[i + 1];
            int x1 = x_grid_[j], x2 = x_grid_[j + 1];

            // patch_diff = diff[y1:y2, x1:x2]
            cv::Mat patch_diff = diff(cv::Range(y1, y2), cv::Range(x1, x2));
            // mean_diff = np.mean(patch_diff)
            // numpy.mean on a uint8 array promotes to float64.
            cv::Scalar mean_scalar = cv::mean(patch_diff);
            double mean_diff = mean_scalar[0];

            int idx = i * GRID_SIZE + j;

            // if mean_diff > self.moving_THRESHOLD:
            //     self.patch_states[i, j] = 1
            if (mean_diff > moving_threshold_) {
                patch_states_[idx] = 1;
            } else {
                // else: if self.patch_states[i, j] == 1:
                //           self.patch_static_counter[i, j] += 1
                //           if self.patch_static_counter[i, j] >= self.STATIC_SCORE_THRESHOLD:
                //               self.patch_states[i, j] = 0
                //               self.patch_static_counter[i, j] = 0
                //               self.static_frame[y1:y2, x1:x2] = current_frame[y1:y2, x1:x2]
                if (patch_states_[idx] == 1) {
                    patch_static_counter_[idx] += 1;
                    if (patch_static_counter_[idx] >= STATIC_SCORE_THRESHOLD) {
                        patch_states_[idx] = 0;
                        patch_static_counter_[idx] = 0;
                        current_frame(cv::Range(y1, y2), cv::Range(x1, x2))
                            .copyTo(static_frame_(cv::Range(y1, y2), cv::Range(x1, x2)));
                    }
                }
            }
        }
    }

    // self.prev_gray = gray.copy()
    prev_gray_ = gray.clone();
    // return self.static_frame
    return static_frame_;
}

} // namespace fall
