// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "roialign.h"
#include <math.h>
#include <algorithm>

namespace ncnn {

DEFINE_LAYER_CREATOR(ROIAlign)

// adapted from detectron2
// https://github.com/facebookresearch/detectron2/blob/master/detectron2/layers/csrc/ROIAlign/ROIAlign_cpu.cpp
template <typename T>
struct PreCalc {
  int pos1;
  int pos2;
  int pos3;
  int pos4;
  T w1;
  T w2;
  T w3;
  T w4;
};

template <typename T>
void pre_calc_for_bilinear_interpolate(
    const int height,
    const int width,
    const int pooled_height,
    const int pooled_width,
    const int iy_upper,
    const int ix_upper,
    T roi_start_h,
    T roi_start_w,
    T bin_size_h,
    T bin_size_w,
    int roi_bin_grid_h,
    int roi_bin_grid_w,
    std::vector<PreCalc<T>>& pre_calc) {
  int pre_calc_index = 0;
  for (int ph = 0; ph < pooled_height; ph++) {
    for (int pw = 0; pw < pooled_width; pw++) {
      for (int iy = 0; iy < iy_upper; iy++) {
        const T yy = roi_start_h + ph * bin_size_h +
            static_cast<T>(iy + .5f) * bin_size_h /
                static_cast<T>(roi_bin_grid_h); // e.g., 0.5, 1.5
        for (int ix = 0; ix < ix_upper; ix++) {
          const T xx = roi_start_w + pw * bin_size_w +
              static_cast<T>(ix + .5f) * bin_size_w /
                  static_cast<T>(roi_bin_grid_w);

          T x = xx;
          T y = yy;
          // deal with: inverse elements are out of feature map boundary
          if (y < -1.0 || y > height || x < -1.0 || x > width) {
            // empty
            PreCalc<T> pc;
            pc.pos1 = 0;
            pc.pos2 = 0;
            pc.pos3 = 0;
            pc.pos4 = 0;
            pc.w1 = 0;
            pc.w2 = 0;
            pc.w3 = 0;
            pc.w4 = 0;
            pre_calc[pre_calc_index] = pc;
            pre_calc_index += 1;
            continue;
          }

          if (y <= 0) {
            y = 0;
          }
          if (x <= 0) {
            x = 0;
          }

          int y_low = (int)y;
          int x_low = (int)x;
          int y_high;
          int x_high;

          if (y_low >= height - 1) {
            y_high = y_low = height - 1;
            y = (T)y_low;
          } else {
            y_high = y_low + 1;
          }

          if (x_low >= width - 1) {
            x_high = x_low = width - 1;
            x = (T)x_low;
          } else {
            x_high = x_low + 1;
          }

          T ly = y - y_low;
          T lx = x - x_low;
          T hy = 1. - ly, hx = 1. - lx;
          T w1 = hy * hx, w2 = hy * lx, w3 = ly * hx, w4 = ly * lx;

          // save weights and indices
          PreCalc<T> pc;
          pc.pos1 = y_low * width + x_low;
          pc.pos2 = y_low * width + x_high;
          pc.pos3 = y_high * width + x_low;
          pc.pos4 = y_high * width + x_high;
          pc.w1 = w1;
          pc.w2 = w2;
          pc.w3 = w3;
          pc.w4 = w4;
          pre_calc[pre_calc_index] = pc;

          pre_calc_index += 1;
        }
      }
    }
  }
}


ROIAlign::ROIAlign()
{
}

int ROIAlign::load_param(const ParamDict& pd)
{
    pooled_width = pd.get(0, 0);
    pooled_height = pd.get(1, 0);
    spatial_scale = pd.get(2, 1.f);
    sampling_ratio = pd.get(3, 0);
    aligned = pd.get(4, true);

    return 0;
}

int ROIAlign::forward(const std::vector<Mat>& bottom_blobs, std::vector<Mat>& top_blobs, const Option& opt) const
{
    const Mat& bottom_blob = bottom_blobs[0];
    const int width = bottom_blob.w;
    const int height = bottom_blob.h;
    const size_t elemsize = bottom_blob.elemsize;
    const int channels = bottom_blob.c;

    const Mat& roi_blob = bottom_blobs[1];

    Mat& top_blob = top_blobs[0];
    top_blob.create(pooled_width, pooled_height, channels, elemsize, opt.blob_allocator);
    if (top_blob.empty())
        return -100;

    const float offset = aligned ? 0.5f : 0.0f;
    // For each ROI R = [x y w h]: max pool over R
    const float* roi_ptr = roi_blob;

    const float roi_start_w = roi_ptr[0] * spatial_scale - offset;
    const float roi_start_h = roi_ptr[1] * spatial_scale - offset;
    const float roi_end_w = roi_ptr[2] * spatial_scale - offset;
    const float roi_end_h = roi_ptr[3] * spatial_scale - offset;

    float roi_width = roi_end_w - roi_start_w;
    float roi_height = roi_end_h - roi_start_h;

    if (!aligned) {
      roi_width = std::max(roi_width, 1.f);
      roi_height = std::max(roi_height, 1.f);
    }

    float bin_size_w = (float)roi_width / (float)pooled_width;
    float bin_size_h = (float)roi_height / (float)pooled_height;

    int roi_bin_grid_h = sampling_ratio > 0 ?
      sampling_ratio : std::ceil(roi_height / pooled_height);
    int roi_bin_grid_w = sampling_ratio > 0 ?
      sampling_ratio : std::ceil(roi_width / pooled_width);

    const float count = std::max(roi_bin_grid_h * roi_bin_grid_w, 1);

    std::vector<PreCalc<float>> pre_calc(
        roi_bin_grid_h * roi_bin_grid_w * pooled_width * pooled_height);
    pre_calc_for_bilinear_interpolate(
        height,
        width,
        pooled_height,
        pooled_width,
        roi_bin_grid_h,
        roi_bin_grid_w,
        roi_start_h,
        roi_start_w,
        bin_size_h,
        bin_size_w,
        roi_bin_grid_h,
        roi_bin_grid_w,
        pre_calc);


    #pragma omp parallel for num_threads(opt.num_threads)
    for (int q=0; q<channels; q++)
    {
        const float* ptr = bottom_blob.channel(q);
        float* outptr = top_blob.channel(q);
        int pre_calc_index = 0;

        for (int ph = 0; ph < pooled_height; ph++)
        {
            for (int pw = 0; pw < pooled_width; pw++)
            {
                float output_val = 0.f;
                for (int iy = 0; iy < roi_bin_grid_h; iy++)
                {
                    for (int ix = 0; ix < roi_bin_grid_w; ix++)
                    {
                        PreCalc<float> pc = pre_calc[pre_calc_index];

                        output_val += pc.w1 * ptr[pc.pos1] +
                            pc.w2 * ptr[pc.pos2] +
                            pc.w3 * ptr[pc.pos3] + pc.w4 * ptr[pc.pos4];

                        pre_calc_index += 1;
                    }
                }
                output_val /= count;
                outptr[pw] = output_val;
            }
            outptr += pooled_width;
        }
    }

    return 0;
}

} // namespace ncnn
