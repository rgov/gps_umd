#include <libgpsmm.h>

// gps.h defines some macros which conflict with enum values in NavSatStatus.h,
// so we map them to new names before including other headers.
#ifdef STATUS_FIX
  enum {
    GPSD_STATUS_NO_FIX   = STATUS_NO_FIX,
    GPSD_STATUS_FIX      = STATUS_FIX,
    GPSD_STATUS_DGPS_FIX = STATUS_DGPS_FIX,
  };
  #undef STATUS_NO_FIX
  #undef STATUS_FIX
  #undef STATUS_DGPS_FIX
#else
  // Renamed in gpsd >= 3.23.1 (commits d4a4d8d3, af3b7dc0, 7d7b889f) without
  // revising the API version number.
  enum {
    GPSD_STATUS_NO_FIX   = STATUS_UNK,
    GPSD_STATUS_FIX      = STATUS_GPS,
    GPSD_STATUS_DGPS_FIX = STATUS_DGPS,
  };
#endif

#include <ros/ros.h>
#include <gps_common/GPSFix.h>
#include <gps_common/GPSStatus.h>
#include <sensor_msgs/NavSatFix.h>
#include <sensor_msgs/NavSatStatus.h>

#include <cstdlib>
#include <cmath>

using namespace gps_common;
using namespace sensor_msgs;

class GPSDClient {
  public:
    GPSDClient() : privnode("~"), gps(NULL), use_gps_time(true), check_fix_by_variance(true), frame_id("gps") {}

    bool start() {
      gps_fix_pub = node.advertise<GPSFix>("extended_fix", 1);
      navsat_fix_pub = node.advertise<NavSatFix>("fix", 1);

      privnode.getParam("use_gps_time", use_gps_time);
      privnode.getParam("check_fix_by_variance", check_fix_by_variance);
      privnode.param("frame_id", frame_id, frame_id);

      std::string host = "localhost";
      int port = atoi(DEFAULT_GPSD_PORT);
      privnode.getParam("host", host);
      privnode.getParam("port", port);

      char port_s[12];
      snprintf(port_s, sizeof(port_s), "%d", port);

      gps_data_t *resp = NULL;

#if GPSD_API_MAJOR_VERSION >= 5
      gps = new gpsmm(host.c_str(), port_s);
      resp = gps->stream(WATCH_ENABLE);
#elif GPSD_API_MAJOR_VERSION == 4
      gps = new gpsmm();
      gps->open(host.c_str(), port_s);
      resp = gps->stream(WATCH_ENABLE);
#else
      gps = new gpsmm();
      resp = gps->open(host.c_str(), port_s);
      gps->query("w\n");
#endif

      if (resp == NULL) {
        ROS_ERROR("Failed to open GPSd");
        return false;
      }

      ROS_INFO("GPSd opened");
      return true;
    }

    void step() {
#if GPSD_API_MAJOR_VERSION >= 5
      if (!gps->waiting(1e6))
        return;

      gps_data_t *p = gps->read();
#else
      gps_data_t *p = gps->poll();
#endif
      process_data(p);
    }

    void stop() {
      delete gps;
    }

  private:
    ros::NodeHandle node;
    ros::NodeHandle privnode;
    ros::Publisher gps_fix_pub;
    ros::Publisher navsat_fix_pub;
    gpsmm *gps;

    bool use_gps_time;
    bool check_fix_by_variance;
    std::string frame_id;

    void process_data(struct gps_data_t* p) {
      if (p == NULL)
        return;

#if GPSD_API_MAJOR_VERSION >= 9
      if (!p->online.tv_sec && !p->online.tv_nsec) {
#else
      if (!p->online) {
#endif
        return;
      }

      process_data_gps(p);
      process_data_navsat(p);
    }


#if GPSD_API_MAJOR_VERSION >= 4
#define SATS_VISIBLE p->satellites_visible
#elif GPSD_API_MAJOR_VERSION == 3
#define SATS_VISIBLE p->satellites
#else
#error "gpsd_client only supports gpsd API versions 3+"
#endif

    void process_data_gps(struct gps_data_t* p) {
      ros::Time time = ros::Time::now();

      GPSFix fix;
      GPSStatus status;

      status.header.stamp = time;
      fix.header.stamp = time;
      fix.header.frame_id = frame_id;

      status.satellites_used = p->satellites_used;

      status.satellite_used_prn.resize(status.satellites_used);
      for (int i = 0; i < status.satellites_used; ++i) {
#if GPSD_API_MAJOR_VERSION > 5
        status.satellite_used_prn[i] = p->skyview[i].used;
#else
        status.satellite_used_prn[i] = p->used[i];
#endif
      }

      status.satellites_visible = SATS_VISIBLE;

      status.satellite_visible_prn.resize(status.satellites_visible);
      status.satellite_visible_z.resize(status.satellites_visible);
      status.satellite_visible_azimuth.resize(status.satellites_visible);
      status.satellite_visible_snr.resize(status.satellites_visible);

      for (int i = 0; i < SATS_VISIBLE; ++i) {
#if GPSD_API_MAJOR_VERSION > 5
        status.satellite_visible_prn[i] = p->skyview[i].PRN;
        status.satellite_visible_z[i] = p->skyview[i].elevation;
        status.satellite_visible_azimuth[i] = p->skyview[i].azimuth;
        status.satellite_visible_snr[i] = p->skyview[i].ss;
#else
        status.satellite_visible_prn[i] = p->PRN[i];
        status.satellite_visible_z[i] = p->elevation[i];
        status.satellite_visible_azimuth[i] = p->azimuth[i];
        status.satellite_visible_snr[i] = p->ss[i];
#endif
      }

#if GPSD_API_MAJOR_VERSION >= 10
      if ((p->fix.status & GPSD_STATUS_FIX) && !(check_fix_by_variance && std::isnan(p->fix.epx))) {
#else
      if ((p->status & GPSD_STATUS_FIX) && !(check_fix_by_variance && std::isnan(p->fix.epx))) {
#endif

        status.status = GPSStatus::STATUS_FIX;

// STATUS_DGPS_FIX was removed in API version 6 but re-added afterward
#if GPSD_API_MAJOR_VERSION != 6
#if GPSD_API_MAJOR_VERSION >= 10
        if (p->fix.status & GPSD_STATUS_DGPS_FIX)
#else
        if (p->status & GPSD_STATUS_DGPS_FIX)
#endif
          status.status |= GPSStatus::STATUS_DGPS_FIX;
#endif

#if GPSD_API_MAJOR_VERSION >= 9
        fix.time = (double)(p->fix.time.tv_sec) + (double)(p->fix.time.tv_nsec) / 1000000.;
#else
        fix.time = p->fix.time;
#endif
        fix.latitude = p->fix.latitude;
        fix.longitude = p->fix.longitude;
        fix.altitude = p->fix.altitude;
        fix.track = p->fix.track;
        fix.speed = p->fix.speed;
        fix.climb = p->fix.climb;

#if GPSD_API_MAJOR_VERSION > 3
        fix.pdop = p->dop.pdop;
        fix.hdop = p->dop.hdop;
        fix.vdop = p->dop.vdop;
        fix.tdop = p->dop.tdop;
        fix.gdop = p->dop.gdop;
#else
        fix.pdop = p->pdop;
        fix.hdop = p->hdop;
        fix.vdop = p->vdop;
        fix.tdop = p->tdop;
        fix.gdop = p->gdop;
#endif

#if GPSD_API_MAJOR_VERSION < 8
        fix.err = p->epe;
#else
        fix.err = p->fix.eph;
#endif
        fix.err_vert = p->fix.epv;
        fix.err_track = p->fix.epd;
        fix.err_speed = p->fix.eps;
        fix.err_climb = p->fix.epc;
        fix.err_time = p->fix.ept;

        /* TODO: attitude */
      } else {
        status.status = GPSStatus::STATUS_NO_FIX;
      }

      fix.status = status;

      gps_fix_pub.publish(fix);
    }

    void process_data_navsat(struct gps_data_t* p) {
      NavSatFix fix;

      /* TODO: Support SBAS and other GBAS. */

#if GPSD_API_MAJOR_VERSION >= 9
      if (use_gps_time && (p->online.tv_sec || p->online.tv_nsec)) {
        fix.header.stamp = ros::Time(p->fix.time.tv_sec, p->fix.time.tv_nsec);
#else
      if (use_gps_time && !std::isnan(p->fix.time)) {
        fix.header.stamp = ros::Time(p->fix.time);
#endif
      }
      else {
        fix.header.stamp = ros::Time::now();
      }

      fix.header.frame_id = frame_id;

#if GPSD_API_MAJOR_VERSION >= 10
      switch (p->fix.status) {
#else
      switch (p->status) {
#endif
        case GPSD_STATUS_NO_FIX:
          fix.status.status = NavSatStatus::STATUS_NO_FIX;
          break;
        case GPSD_STATUS_FIX:
          fix.status.status = NavSatStatus::STATUS_FIX;
          break;
// STATUS_DGPS_FIX was removed in API version 6 but re-added afterward
#if GPSD_API_MAJOR_VERSION != 6
        case GPSD_STATUS_DGPS_FIX:
          fix.status.status = NavSatStatus::STATUS_GBAS_FIX;
          break;
#endif
      }

      fix.status.service = NavSatStatus::SERVICE_GPS;

      fix.latitude = p->fix.latitude;
      fix.longitude = p->fix.longitude;
      fix.altitude = p->fix.altitude;

      /* gpsd reports status=OK even when there is no current fix,
       * as long as there has been a fix previously. Throw out these
       * fake results, which have NaN variance
       */
      if (std::isnan(p->fix.epx) && check_fix_by_variance) {
        ROS_DEBUG_THROTTLE(1,
            "GPS status was reported as OK, but variance was invalid");
        return;
      }

      fix.position_covariance[0] = p->fix.epx;
      fix.position_covariance[4] = p->fix.epy;
      fix.position_covariance[8] = p->fix.epv;

      fix.position_covariance_type = std::isnan(p->fix.epx) ?
        NavSatFix::COVARIANCE_TYPE_UNKNOWN :
        NavSatFix::COVARIANCE_TYPE_DIAGONAL_KNOWN;

      navsat_fix_pub.publish(fix);
    }
};

int main(int argc, char ** argv) {
  ros::init(argc, argv, "gpsd_client");

  GPSDClient client;

  if (!client.start())
    return -1;


  while(ros::ok()) {
    ros::spinOnce();
    client.step();
  }

  client.stop();
}
