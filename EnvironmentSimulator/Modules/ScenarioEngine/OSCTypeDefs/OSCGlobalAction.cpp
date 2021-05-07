/*
 * esmini - Environment Simulator Minimalistic
 * https://github.com/esmini/esmini
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Copyright (c) partners of Simulation Scenarios
 * https://sites.google.com/view/simulationscenarios
 */

#include "OSCGlobalAction.hpp"
#include "OSCSwarmTrafficGeometry.hpp"
#include <memory>
#include <cmath>
#include <iostream>
#include <fstream>

using namespace scenarioengine;
using namespace STGeometry;
using aabbTree::BBoxVec;
using aabbTree::ptTriangle;
using aabbTree::ptBBox;
using std::make_shared;
using aabbTree::BBox;
using std::vector;

#define USELESS_THRESHOLD 5 // Max check count before deleting uneffective vehicles
#define VHEICLE_DISTANCE 12 // Min distance between two spawned vehicles
#define TIME_INTERVAL 0.1   // Sleep time between two excution of a step

void ParameterSetAction::Start()
{
    LOG("Set parameter %s = %s", name_.c_str(), value_.c_str());
    parameters_->setParameterValue(name_, value_);
    OSCAction::Start();
}

void ParameterSetAction::Step(double dt, double simTime)
{
    OSCAction::Stop();
}

void print_triangles(BBoxVec &vec, char const filename[]) {
    std::ofstream file;
    file.open(filename);
    for (auto const bbx : vec) {
        auto trPtr = bbx->triangle();
        auto pt = trPtr->a;
        file << pt.x << "," << pt.y;
        pt = trPtr->b;
        file << "," << pt.x << "," << pt.y;
        pt = trPtr->c;
        file << "," << pt.x << "," << pt.y << "\n";
    }
    file.close();
}

void print_bbx(BBoxVec &vec, char const filename[]) {
    std::ofstream file;
    file.open(filename);
    for (auto const bbx : vec) {
        auto pt = bbx->blhCorner();
        file << pt.x << "," << pt.y;
        pt = bbx->urhCorner();
        file << "," << pt.x << "," << pt.y << "\n";
    }
    file.close();
}

void printTree(aabbTree::Tree &tree, char filename[]) {
    std::ofstream file;
    file.open(filename);
    std::vector<aabbTree::ptTree> v1, v2;
    v1.clear(); v2.clear();
    
    if (tree.empty()) {
        file.close();
        return;
    }

    auto bbx = tree.BBox();
    file << bbx->blhCorner().x << "," << bbx->blhCorner().y << 
            "," << bbx->urhCorner().x << "," << bbx->urhCorner().y << "\n";
    v1.insert(v1.end(), tree.Children().begin(), tree.Children().end());

    while (!v1.empty()) {
        for (auto const tr : v1) {
            if (!tr->empty()) {
                auto bbox = tr->BBox();
                file << bbox->blhCorner().x << "," << bbox->blhCorner().y << 
                        "," << bbox->urhCorner().x << "," << bbox->urhCorner().y << ",";
                v2.insert(v2.end(), tr->Children().begin(), tr->Children().end());
            }
        }
        file << "\n";
        v1.clear();
        v1 = v2;
        v2.clear();
    }

    file.close();
}

void SwarmTrafficAction::Start()
{
    LOG("SwarmTrafficAction Start");
    printf("IR: %f, SMjA: %f, SMnA: %f, maxV: %i\n", innerRadius_, semiMajorAxis_, semiMinorAxis_, numberOfVehicles);
    double x0, y0, x1, y1;

    midSMjA = (semiMajorAxis_ + innerRadius_) / 2.0;
    midSMnA = (semiMinorAxis_ + innerRadius_) / 2.0;
    lastTime = -1;

    paramEllipse(0, 0, 0, midSMjA, midSMnA, 0, x0, y0);
    paramEllipse(M_PI / 36, 0, 0, midSMjA, midSMnA, 0, x1, y1);
    
    odrManager_ = roadmanager::Position::GetOpenDrive();
    minSize_    = ceil(sqrt(pow(x1 - x0, 2) + pow(y1 - y0, 2)) * 100) / 100.0;
    if (minSize_ == 0) minSize_ = 1.0;
    
    aabbTree::ptTree tree = std::make_shared<aabbTree::Tree>();
    aabbTree::BBoxVec vec;
    vec.clear();
    createRoadSegments(vec);

    tree->build(vec);
    rTree = tree;
    OSCAction::Start();
}

void SwarmTrafficAction::Step(double dt, double simTime) 
{
    // Executes the step at each TIME_INTERVAL
    if (lastTime < 0 || abs(simTime - lastTime) > TIME_INTERVAL) {
        LOG("SwarmTrafficAction Step");

        double SMjA = midSMjA;
        double SMnA = midSMnA;

        BBoxVec vec;
        aabbTree::Candidates candidates;
        std::vector<ptTriangle> triangle;
        Solutions sols;
        vec.clear(); candidates.clear(); triangle.clear(); sols.clear();
    
        EllipseInfo info = {
            SMjA,
            SMnA,
            centralObject_->pos_
        };
    
        createEllipseSegments(vec, SMjA, SMnA);
        aabbTree::Tree eTree;
        eTree.build(vec);
        rTree->intersect(eTree, candidates);
        aabbTree::processCandidates(candidates, triangle);
        aabbTree::findPoints(triangle, info, sols);
        printf("N points found: %d\n", sols.size());
    
        spawn(sols, despawn(simTime), simTime);
        lastTime = simTime;
    }
}

void SwarmTrafficAction::createRoadSegments(BBoxVec &vec) 
{
    for (int i = 0; i < odrManager_->GetNumOfRoads(); i++) {
        roadmanager::Road* road = odrManager_->GetRoadByIdx(i);
        for (size_t i = 0; i < road->GetNumberOfGeometries(); i++) {
            roadmanager::Geometry *gm = road->GetGeometry(i);
            switch (gm->GetType()) {
                case gm->GEOMETRY_TYPE_UNKNOWN: {
                    break;
                }
                case gm->GEOMETRY_TYPE_LINE: {
                    auto const length = gm->GetLength();
                    for (double dist = gm->GetS(); dist < length;) {
                        double ds = dist + minSize_;
                        if (ds > length)
                            ds = length;
                        double x0, y0, x1, y1, x2, y2, dummy, l;
                        gm->EvaluateDS(dist, &x0, &y0, &dummy);
                        gm->EvaluateDS(ds, &x1, &y1, &dummy);
                        l = sqrt(pow(x1 - x0, 2) + pow(y1 - y0, 2));
                        x2 = (x1 + x0)/2 + l / 4.0;
                        y2 = (y1 + y0)/2 + l / 4.0;
                        Point a(x0, y0), b(x1, y1), c(x2, y2);
                        ptTriangle triangle = make_shared<Triangle>(gm);
                        triangle->a = a;
                        triangle->b = b;
                        triangle->c = c;
                        triangle->sI = dist;
                        triangle->sF = ds;
                        ptBBox bbox = make_shared<BBox>(triangle);
                        vec.push_back(bbox);
                        dist = ds;
                    }
                    break;
                }
                case gm->GEOMETRY_TYPE_ARC: {
                    break;
                }
                case gm->GEOMETRY_TYPE_SPIRAL: {
                    break;
                }
                case gm->GEOMETRY_TYPE_POLY3: {
                    break;
                }
                case gm->GEOMETRY_TYPE_PARAM_POLY3: {
                    break;
                }
            }
        }
    }
}

void SwarmTrafficAction::createEllipseSegments(aabbTree::BBoxVec &vec, double SMjA, double SMnA) 
{
    double alpha = -M_PI / 72.0;
    double dAlpha = M_PI / 36.0;
    auto pos = centralObject_->pos_;
    double x0, y0, x1, y1, x2, y2;
    while (alpha < (2 * M_PI - M_PI / 72.0)) {

        double da = alpha + dAlpha;
        if (da > 2 * M_PI - M_PI / 72.0)
            da = 2 * M_PI - M_PI / 72.0;

        paramEllipse(alpha , pos.GetX(), pos.GetY(), SMjA, SMnA, pos.GetH(), x0, y0);
        paramEllipse(da, pos.GetX(), pos.GetY(), SMjA, SMnA, pos.GetH(), x1, y1);

        double dPx, dPy, theta0, theta1, r0x, r0y, r1x, r1y, s;
        dPx = x1 - x0;
        dPy = y1 - y0;
        theta0 = angleTangentEllipse(SMjA, SMnA, alpha, pos.GetH());
        theta1 = angleTangentEllipse(SMjA, SMnA, da, pos.GetH());
        
        r0x = cos(theta0);
        r0y = sin(theta0);
        r1x = cos(theta1);
        r1y = sin(theta1);

        s = (dPy * r1x - dPx * r1y) / (r1x * r0y - r0x * r1y);

        x2 = x0 + s * r0x;
        y2 = y0 + s * r0y;

        ptTriangle triangle = make_shared<Triangle>();
        triangle->a = Point(x0, y0);
        triangle->b = Point(x1, y1);
        triangle->c = Point(x2, y2);
        vec.push_back(make_shared<BBox>(triangle));
        
        alpha = da;
    }
}

inline Vehicle*
createVehicle(roadmanager::Position pos, char hdg_offset, int lane, double speed, scenarioengine::Controller *controller, std::string model_filepath) 
{
    Vehicle* vehicle = new Vehicle();
    vehicle->pos_.SetInertiaPos(pos.GetX(), pos.GetY(), pos.GetH() + hdg_offset * M_PI, true);
    vehicle->pos_.SetLanePos(vehicle->pos_.GetTrackId(), lane, vehicle->pos_.GetS(), 0);
    vehicle->SetSpeed(speed);
    vehicle->controller_     = controller;
    vehicle->model_filepath_ = model_filepath;
    return vehicle;
}

void SwarmTrafficAction::spawn(Solutions sols, int replace, double simTime) 
{   
    for (Point &pt : sols) {
        for (int lane : {1, -1}) {
            if (spawnedV.size() >= numberOfVehicles) return;

            roadmanager::Position pos;
            pos.XYZH2TrackPos(pt.x, pt.y, 0, pt.h);
            if (!ensureDistance(pos, lane)) continue;

            Vehicle* vehicle;
            vehicle = createVehicle(pos, (lane < 0 ? 0 : 1), lane, centralObject_->GetSpeed(), NULL, centralObject_->model_filepath_);
            int id          = entities_->addObject(vehicle);
            vehicle->name_  = std::to_string(id); 
            SpawnInfo sInfo = {
                id,                // Vehicle ID
                0,                 // Useless detection counter
                pos.GetTrackId(),  // Road ID
                lane,              // Lane
                simTime            // Simulation time
            };
            spawnedV.push_back(sInfo);
        }
    }
}

bool SwarmTrafficAction::ensureDistance(roadmanager::Position pos, int lane) 
{
    for (SpawnInfo info: spawnedV) {
        if (info.lane == lane && info.roadID == pos.GetTrackId()) {
            Object *vehicle = entities_->GetObjectById(info.vehicleID);
            if (abs(vehicle->pos_.GetS() - pos.GetS()) <= VHEICLE_DISTANCE)
                return false;
        }
    }   
    return true;
}

int SwarmTrafficAction::despawn(double simTime) 
{
    auto infoPtr               = spawnedV.begin();
    bool increase              = true;
    bool deleteVehicle         = false;
    int count                  = 0;
    roadmanager::Position cPos = centralObject_->pos_;

    while (infoPtr < spawnedV.end()) {
        Object *vehicle = entities_->GetObjectById(infoPtr->vehicleID);
        roadmanager::Position vPos = vehicle->pos_;
        auto e0 = ellipse(cPos.GetX(), cPos.GetY(), cPos.GetH(), semiMajorAxis_, semiMinorAxis_, vPos.GetX(), vPos.GetY());
        auto e1 = ellipse(cPos.GetX(), cPos.GetY(), cPos.GetH(), midSMjA, midSMnA, vPos.GetX(), vPos.GetY());

        if (e0 > 0.001) // outside major ellipse
            deleteVehicle = true;
        else if (e1 > 0.001 || (0 <= e1 && e1 <= 0.001)) // outside middle ellipse or on the border
        {
            infoPtr->outMidAreaCount++;
            if (infoPtr->outMidAreaCount > USELESS_THRESHOLD)
                deleteVehicle = true;
        } else 
            infoPtr->outMidAreaCount = 0;

        if (deleteVehicle) {
            entities_->removeObject(vehicle->name_);
            delete vehicle;
            infoPtr = spawnedV.erase(infoPtr);
            increase = deleteVehicle = false;
            count++;
        }

        if (increase) ++infoPtr;
        increase = true;
    }
    return count;
}