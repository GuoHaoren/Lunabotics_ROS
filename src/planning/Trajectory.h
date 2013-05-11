#ifndef _TRAJECTORY_H_
#define _TRAJECTORY_H_

#include "../types.h"
#include "../geometry/BezierCurve.h"

namespace lunabotics {

struct TrajectorySegment {
	int start_idx;
	int finish_idx;
	BezierCurvePtr curve;
};

typedef std::vector<TrajectorySegment> TrajectorySegmentArr;

class Trajectory {
private:
	PointArr _cached_points;
	TrajectorySegmentArr _segments;
	float _cached_max_curvature;
	
	void freeSegments();
public:
	Trajectory();
	~Trajectory();
	
	TrajectorySegmentArr segments();
	void setSegments(TrajectorySegmentArr segments);
	void appendSegment(TrajectorySegment s);
	PointArr getPoints();
	float maxCurvature();
};

typedef Trajectory * TrajectoryPtr;

}


#endif
