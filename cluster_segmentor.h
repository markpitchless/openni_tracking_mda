#pragma once

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/common/time.h>
#include <pcl/common/centroid.h>
#include <pcl/io/pcd_io.h>

#include <pcl/filters/passthrough.h>
#include <pcl/filters/project_inliers.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/approximate_voxel_grid.h>
#include <pcl/filters/extract_indices.h>

#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_polygonal_prism_data.h>
#include <pcl/segmentation/extract_clusters.h>

#include <pcl/surface/convex_hull.h>


template<typename PointType>
class ClusterSegmentor
{
  protected:
    typedef pcl::PointCloud<PointType> Cloud;
    typedef typename pcl::PointCloud<PointType>::Ptr CloudPtr;
    typedef typename pcl::PointCloud<PointType>::ConstPtr CloudConstPtr;
    typedef typename pcl::search::KdTree<PointType> KdTree;
    typedef typename KdTree::Ptr KdTreePtr;

  public:
    /** \brief Empty constructor.
      */
    ClusterSegmentor()
      : use_convex_hull_(true)
    {}

    /** \brief Destructor.
      */
    virtual
    ~ClusterSegmentor()
    {
      input_.reset();
      target_cloud_.reset();
      nonplane_cloud_.reset();
      cloud_hull_.reset();
    }

    /** \brief Provide a pointer to the input dataset
      * \param cloud the const boost shared pointer to a PointCloud message
      */
    virtual inline void
    setInputCloud (const CloudConstPtr &cloud) { input_ = cloud; }

    /** \brief Get a pointer to the input point cloud dataset. */
    inline CloudConstPtr const
    getInputCloud () { return (input_); }

    /**
     * \breif Segment the target_cloud into clusters, pushing each cluster onto the results vector.
     */
    void extract (std::vector<CloudPtr> &results)
    {
      PCL_INFO("segmenting clusters...\n");

      segmentTargetCloud(); // Sets target_cloud_

      std::vector<pcl::PointIndices> cluster_indices;
      euclideanSegment (cluster_indices);
      if (cluster_indices.size () == 0)
        return;

      CloudPtr temp_cloud;
      for (size_t i = 0; i < cluster_indices.size (); i++)
      {
        temp_cloud.reset (new Cloud);
        extractSegmentCluster (cluster_indices, i, *temp_cloud);
        results.push_back(temp_cloud);
      }
    }

    void
    extractByDistance (std::vector<CloudPtr> &results)
    {
      extract (results);
      std::sort (results.begin(), results.end(), ClusterSegmentor::byDistance);
    }

    static bool
    byDistance (const CloudPtr &a, const CloudPtr &b)
    {
      Eigen::Vector4f ca, cb;
      pcl::compute3DCentroid<PointType> (*a, ca);
      pcl::compute3DCentroid<PointType> (*b, cb);
      //double da = ca[0] * ca[0] + ca[1] * ca[1];
      double da = ca[0] * ca[0] + ca[1] * ca[1] + ca[2] * ca[2];
      double db = cb[0] * cb[0] + cb[1] * cb[1] + cb[2] * cb[2];
      return (da < db);
    }

  protected:
    virtual void
    euclideanSegment (std::vector<pcl::PointIndices> &cluster_indices)
    {
      pcl::EuclideanClusterExtraction<PointType> ec;
      KdTreePtr tree (new KdTree ());

      ec.setClusterTolerance (0.05); // 2cm
      ec.setMinClusterSize (50);
      ec.setMaxClusterSize (25000);
      ec.setSearchMethod (tree);
      ec.setInputCloud (target_cloud_);
      ec.extract (cluster_indices);
    }

    void
    extractSegmentCluster (
        const std::vector<pcl::PointIndices> cluster_indices,
        const int segment_index,
        Cloud &result)
    {
      pcl::PointIndices segmented_indices = cluster_indices[segment_index];
      for (size_t i = 0; i < segmented_indices.indices.size (); i++)
      {
        PointType point = target_cloud_->points[segmented_indices.indices[i]];
        result.points.push_back (point);
      }
      result.width = result.points.size ();
      result.height = 1;
      result.is_dense = true;
    }

    /**
     * Pull out a useful cloud to search for trackable objects in. Sets target_cloud_.
     */
    void segmentTargetCloud()
    {
      target_cloud_.reset (new Cloud);
      pcl::ModelCoefficients::Ptr coefficients (new pcl::ModelCoefficients ());
      pcl::PointIndices::Ptr inliers (new pcl::PointIndices ());

      if (use_convex_hull_)
      {
        planeSegmentation (*coefficients, *inliers);
        if (inliers->indices.size () > 3)
        {
          CloudPtr cloud_projected (new Cloud);
          cloud_hull_.reset (new Cloud);
          nonplane_cloud_.reset (new Cloud);

          planeProjection (*cloud_projected, coefficients);
          convexHull (cloud_projected, *cloud_hull_, hull_vertices_);

          extractNonPlanePoints (input_, cloud_hull_, *nonplane_cloud_);
          target_cloud_ = nonplane_cloud_;
          pcl::io::savePCDFileASCII("segment_target_cloud.pcd", *target_cloud_);
        }
        else
        {
          PCL_WARN("cannot segment plane\n");
        }
      }
      else
      {
        PCL_WARN("without plane segmentation\n");
        target_cloud_ = input_;
      }
    }

    void
    planeSegmentation ( pcl::ModelCoefficients &coefficients, pcl::PointIndices &inliers)
    {
      pcl::SACSegmentation<PointType> seg;
      seg.setOptimizeCoefficients (true);
      seg.setModelType (pcl::SACMODEL_PLANE);
      seg.setMethodType (pcl::SAC_RANSAC);
      seg.setMaxIterations (1000);
      seg.setDistanceThreshold (0.03);
      seg.setInputCloud (input_);
      seg.segment (inliers, coefficients);
    }

    void
    planeProjection (Cloud &result, const pcl::ModelCoefficients::ConstPtr &coefficients)
    {
      pcl::ProjectInliers<PointType> proj;
      proj.setModelType (pcl::SACMODEL_PLANE);
      proj.setInputCloud (input_);
      proj.setModelCoefficients (coefficients);
      proj.filter (result);
    }

    void
    convexHull (const CloudConstPtr &cloud, Cloud &result, std::vector<pcl::Vertices> &hull_vertices)
    {
      pcl::ConvexHull<PointType> chull;
      chull.setInputCloud (cloud);
      chull.reconstruct (*cloud_hull_, hull_vertices);
    }

    void
    extractNonPlanePoints (const CloudConstPtr &cloud, const CloudConstPtr &cloud_hull, Cloud &result)
    {
      pcl::ExtractPolygonalPrismData<PointType> polygon_extract;
      pcl::PointIndices::Ptr inliers_polygon (new pcl::PointIndices ());
      polygon_extract.setHeightLimits (0.01, 10.0);
      polygon_extract.setInputPlanarHull (cloud_hull);
      polygon_extract.setInputCloud (cloud);
      polygon_extract.segment (*inliers_polygon);
      {
        pcl::ExtractIndices<PointType> extract_positive;
        extract_positive.setNegative (false);
        extract_positive.setInputCloud (cloud);
        extract_positive.setIndices (inliers_polygon);
        extract_positive.filter (result);
      }
    }

    bool use_convex_hull_;
    CloudConstPtr input_;
    CloudConstPtr target_cloud_;
    CloudPtr nonplane_cloud_;
    CloudPtr cloud_hull_;
    std::vector<pcl::Vertices> hull_vertices_;
};
