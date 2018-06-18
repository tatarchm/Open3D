// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "PointCloud.h"

#include <unordered_map>

#include <Core/Utility/Helper.h>
#include <Core/Utility/Console.h>

namespace three{

namespace {

class AccumulatedPoint
{
public:
	AccumulatedPoint() :
			num_of_points_(0),
			point_(0.0, 0.0, 0.0),
			normal_(0.0, 0.0, 0.0),
			color_(0.0, 0.0, 0.0)
	{
	}

public:
	void AddPoint(const PointCloud &cloud, int index)
	{
		point_ += cloud.points_[index];
		if (cloud.HasNormals()) {
			if (!std::isnan(cloud.normals_[index](0)) &&
					!std::isnan(cloud.normals_[index](1)) &&
					!std::isnan(cloud.normals_[index](2))) {
				normal_ += cloud.normals_[index];
			}
		}
		if (cloud.HasColors()) {
			color_ += cloud.colors_[index];
		}
		num_of_points_++;
	}

	Eigen::Vector3d GetAveragePoint() const
	{
		return point_ / double(num_of_points_);
	}

	Eigen::Vector3d GetAverageNormal() const
	{
		return normal_.normalized();
	}

	Eigen::Vector3d GetAverageColor() const
	{
		return color_ / double(num_of_points_);
	}

public:
	int num_of_points_;
	Eigen::Vector3d point_;
	Eigen::Vector3d normal_;
	Eigen::Vector3d color_;
};

class point_cubic_id
{
public:
	size_t point_id;
	int cubic_id;
};

class AccumulatedPointForSurfaceConv : public AccumulatedPoint
{
public:
	void AddPoint(const PointCloud &cloud, size_t index,
			int cubic_index, bool approximate_class)
	{
		point_ += cloud.points_[index];
		if (cloud.HasNormals()) {
			if (!std::isnan(cloud.normals_[index](0)) &&
					!std::isnan(cloud.normals_[index](1)) &&
					!std::isnan(cloud.normals_[index](2))) {
				normal_ += cloud.normals_[index];
			}
		}
		if (cloud.HasColors()) {
			if (approximate_class) {
				auto got = classes.find(cloud.colors_[index][0]);
				if (got == classes.end())
					classes[cloud.colors_[index][0]] = 1;
				else classes[cloud.colors_[index][0]] += 1;
			}
			else {
				color_ += cloud.colors_[index];
			}
		}
		point_cubic_id new_id;
		new_id.point_id = index;
		new_id.cubic_id = cubic_index;
		original_id.push_back(new_id);
		num_of_points_++;
	}

	Eigen::Vector3d GetMaxClass()
	{
		int max_class = -1;
		int max_count = -1;
		for(auto it=classes.begin(); it!=classes.end(); it++)
		{
			if(it->second > max_count)
			{
				max_count = it->second;
				max_class = it->first;
			}
		}
		return Eigen::Vector3d(max_class, max_class, max_class);
	}

	std::vector<point_cubic_id> GetOriginalID()
	{
		return original_id;
	}

private:
	// original point cloud id in higher resolution + its cubic id
	std::vector<point_cubic_id> original_id;
	std::unordered_map<int, int> classes;
};

}	// unnamed namespace

std::shared_ptr<PointCloud> SelectDownSample(const PointCloud &input,
		const std::vector<size_t> &indices)
{
	auto output = std::make_shared<PointCloud>();
	bool has_normals = input.HasNormals();
	bool has_colors = input.HasColors();
	for (size_t i : indices) {
		output->points_.push_back(input.points_[i]);
		if (has_normals) output->normals_.push_back(input.normals_[i]);
		if (has_colors) output->colors_.push_back(input.colors_[i]);
	}
	PrintDebug("Pointcloud down sampled from %d points to %d points.\n",
			(int)input.points_.size(), (int)output->points_.size());
	return output;
}

std::shared_ptr<PointCloud> VoxelDownSample(const PointCloud &input,
		double voxel_size)
{
	auto output = std::make_shared<PointCloud>();
	if (voxel_size <= 0.0) {
		PrintDebug("[VoxelDownSample] voxel_size <= 0.\n");
		return output;
	}
	Eigen::Vector3d voxel_size3 =
			Eigen::Vector3d(voxel_size, voxel_size, voxel_size);
	Eigen::Vector3d voxel_min_bound = input.GetMinBound() - voxel_size3 * 0.5;
	Eigen::Vector3d voxel_max_bound = input.GetMaxBound() + voxel_size3 * 0.5;
	if (voxel_size * std::numeric_limits<int>::max() <
			(voxel_max_bound - voxel_min_bound).maxCoeff()) {
		PrintDebug("[VoxelDownSample] voxel_size is too small.\n");
		return output;
	}
	std::unordered_map<Eigen::Vector3i, AccumulatedPoint,
			hash_eigen::hash<Eigen::Vector3i>> voxelindex_to_accpoint;
	Eigen::Vector3d ref_coord;
	Eigen::Vector3i voxel_index;
	for (int i = 0; i < (int)input.points_.size(); i++) {
		ref_coord = (input.points_[i] - voxel_min_bound) / voxel_size;
		voxel_index << int(floor(ref_coord(0))),
				int(floor(ref_coord(1))), int(floor(ref_coord(2)));
		voxelindex_to_accpoint[voxel_index].AddPoint(input, i);
	}
	bool has_normals = input.HasNormals();
	bool has_colors = input.HasColors();
	for (auto accpoint : voxelindex_to_accpoint) {
		output->points_.push_back(accpoint.second.GetAveragePoint());
		if (has_normals) {
			output->normals_.push_back(accpoint.second.GetAverageNormal());
		}
		if (has_colors) {
			output->colors_.push_back(accpoint.second.GetAverageColor());
		}
	}
	PrintDebug("Pointcloud down sampled from %d points to %d points.\n",
			(int)input.points_.size(), (int)output->points_.size());
	return output;
}

std::shared_ptr<PointCloudWithHighResCubicID> VoxelDownSampleForSurfaceConv(
		const PointCloud &input, double voxel_size,
		const Eigen::Vector3d &min_bound, const Eigen::Vector3d &max_bound,
		bool approximate_class)
{
	auto output = std::make_shared<PointCloudWithHighResCubicID>();
	if (voxel_size <= 0.0) {
		PrintDebug("[VoxelDownSample] voxel_size <= 0.\n");
		return output;
	}
	auto voxel_size3 = Eigen::Vector3d(voxel_size, voxel_size, voxel_size);
	// Note: this is different from VoxelDownSample.
	// It is for fixing coordinate for multiscale voxel space
	auto voxel_min_bound = min_bound;
	auto voxel_max_bound = max_bound;
	if (voxel_size * std::numeric_limits<int>::max() <
			(voxel_max_bound - voxel_min_bound).maxCoeff()) {
		PrintDebug("[VoxelDownSample] voxel_size is too small.\n");
		return output;
	}
	std::unordered_map<Eigen::Vector3i, AccumulatedPointForSurfaceConv,
			hash_eigen::hash<Eigen::Vector3i>> voxelindex_to_accpoint;
	int cubic_id_temp[3] = {1, 2, 4};
	for (size_t i = 0; i < input.points_.size(); i++) {
		auto ref_coord = (input.points_[i] - voxel_min_bound) / voxel_size;
		auto voxel_index = Eigen::Vector3i(int(floor(ref_coord(0))),
				int(floor(ref_coord(1))), int(floor(ref_coord(2))));
		// assumes the input point cloud already downsampled using VoxelDownSample.
		int cubic_id = 0;
		for (int c = 0; c < 3; c++){
			if((ref_coord(c)-voxel_index(c)) >= 0.5){
				cubic_id += cubic_id_temp[c];
			}
		}
		voxelindex_to_accpoint[voxel_index].AddPoint(
				input, i, cubic_id, approximate_class);
	}
	bool has_normals = input.HasNormals();
	bool has_colors = input.HasColors();
	int cnt = 0;
	output->cubic_id.resize(voxelindex_to_accpoint.size(),8);
	output->cubic_id.setConstant(-1);
	for (auto accpoint : voxelindex_to_accpoint) {
		output->point_cloud.points_.push_back(
				accpoint.second.GetAveragePoint());
		if (has_normals) {
			output->point_cloud.normals_.push_back(
					accpoint.second.GetAverageNormal());
		}
		if (has_colors) {
			if (approximate_class) {
				output->point_cloud.colors_.push_back(
						accpoint.second.GetMaxClass());
			}
			else {
				output->point_cloud.colors_.push_back(
						accpoint.second.GetAverageColor());
			}
		}
		auto original_id = accpoint.second.GetOriginalID();
		for (int i = 0; i < (int)original_id.size(); i++){
			size_t point_id = original_id[i].point_id;
			int cubic_id = original_id[i].cubic_id;
			output->cubic_id(cnt, cubic_id) = point_id;
		}
		cnt++;
	}
	PrintDebug("Pointcloud down sampled from %d points to %d points.\n",
			(int)input.points_.size(), (int)output->point_cloud.points_.size());
	return output;
}

std::shared_ptr<PointCloud> UniformDownSample(const PointCloud &input,
		size_t every_k_points)
{
	if (every_k_points == 0) {
		PrintDebug("[UniformDownSample] Illegal sample rate.\n");
		return std::make_shared<PointCloud>();
	}
	std::vector<size_t> indices;
	for (size_t i = 0; i < input.points_.size(); i += every_k_points) {
		indices.push_back(i);
	}
	return SelectDownSample(input, indices);
}

std::shared_ptr<PointCloud> CropPointCloud(const PointCloud &input,
		const Eigen::Vector3d &min_bound, const Eigen::Vector3d &max_bound)
{
	if (min_bound(0) > max_bound(0) || min_bound(1) > max_bound(1) ||
			min_bound(2) > max_bound(2)) {
		PrintDebug("[CropPointCloud] Illegal boundary clipped all points.\n");
		return std::make_shared<PointCloud>();
	}
	std::vector<size_t> indices;
	for (size_t i = 0; i < input.points_.size(); i++) {
		const auto &point = input.points_[i];
		if (point(0) >= min_bound(0) && point(0) <= max_bound(0) &&
				point(1) >= min_bound(1) && point(1) <= max_bound(1) &&
				point(2) >= min_bound(2) && point(2) <= max_bound(2)) {
			indices.push_back(i);
		}
	}
	return SelectDownSample(input, indices);
}

}	// namespace three
