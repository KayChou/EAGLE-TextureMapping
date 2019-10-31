﻿#include "getalignresults.h"

/*----------------------------------------------
 *  Log Settings
 * ---------------------------------------------*/
#define LOG_INIT_N true
#define LOG_PM     true
#define LOG_SAVE_T true
#define LOG_SAVE_M true

#define OUTPUT_T_M_INSTANT true

/*----------------------------------------------
 *  To get the position of the patchmatch result
 * ---------------------------------------------*/
#define INT_TO_X(v) ((v)&((1<<12)-1))
#define INT_TO_Y(v) ((v)>>12)

/*----------------------------------------------
 *  Math
 * ---------------------------------------------*/
#define EPS 1e-10
#define EAGLE_MAX(x,y) (x > y ? x : y)
#define EAGLE_MIN(x,y) (x < y ? x : y)

/*----------------------------------------------
 *  Main
 * ---------------------------------------------*/
getAlignResults::getAlignResults(Settings &_settings)
{
    settings = _settings;
    // get all keyframe imgs' full path
    sourcesPath = settings.keyFramesPath;
    EAGLE::checkPath(sourcesPath);
    cv::glob(sourcesPath + "/" + settings.kfRGBMatch, sourcesFiles, false);
    // range of all frames
    kfStart = 0; kfTotal = sourcesFiles.size();
    // range of valid frames
    if( settings.kfIndexs.size() > 0 ) {
        kfIndexs = settings.kfIndexs;
    } else {
        kfIndexs.clear();
        for( size_t i = kfStart; i < kfTotal; i++ )
            kfIndexs.push_back(i);
    }
    // make the dir to store targets
    targetsPath = sourcesPath + "/targets";
    EAGLE::checkPath(targetsPath);
    // make the dir to store textures
    texturesPath = sourcesPath + "/textures";
    EAGLE::checkPath(texturesPath);
    // make the dir to store results
    resultsPath = sourcesPath + "/results";
    EAGLE::checkPath(resultsPath);
    log.open( resultsPath + "/result.log" );
    // make the dir to store pm's results
    pmResultPath = sourcesPath + "/patchmatchs";
    EAGLE::checkPath(pmResultPath);
    LOG("[ From Path: " + settings.keyFramesPath + " ] ");
    LOG("[ Alpha U: " + std::to_string(settings.alpha_u) + " Alpha V: " + std::to_string(settings.alpha_v) + " Lambda: " + std::to_string(settings.lamda) + " ] ");
    //pcl::PolygonMesh mesh;
    pcl::io::loadPLYFile(settings.keyFramesPath + "/" + settings.plyFile, mesh);
    LOG("[ PLY Model: " + std::to_string(mesh.cloud.width) + " vertexs | " + std::to_string(mesh.polygons.size()) + " faces ]");
    // init remappings
    calcVertexMapping();
    LOG("[ Init Success. " + std::to_string(kfIndexs.size()) + " / " + std::to_string(kfTotal) + " Images " + "]");

    doIterations();

    sourcesImgs.clear();
    for ( size_t i = 0; i < kfTotal; i++ )
        sourcesImgs.push_back( cv::imread(sourcesFiles[i]) ); // img.at<cv::Vec3b>(y, x)(0)
    generateColoredPLY(resultsPath, "result_S.ply", sourcesImgs);
    cv::glob(targetsPath + "/" + settings.kfRGBMatch, targetsFiles, false);
    targetsImgs.clear();
    for( size_t i = 0; i < kfTotal; i++ )
       targetsImgs.push_back( cv::imread(targetsFiles[i]) );
    generateColoredPLY(resultsPath, "result_T.ply", targetsImgs);
    LOG("[ Output PLY Success ]");
    LOG("[ End ]");
}
getAlignResults::~getAlignResults()
{
    log.close();

    sourcesImgs.clear();
    targetsImgs.clear();
    texturesImgs.clear();

    weights.clear();
    for( size_t t : kfIndexs ) {
        uvs[t].clear();
        mappings[t].clear();
    }
    uvs.clear();
    mappings.clear();
    img_valid_mesh.clear();
    img_valid_mesh_lamda.clear();
}

/*----------------------------------------------
 *  LOG
 * ---------------------------------------------*/
void getAlignResults::LOG(std::string t, bool nl)
{
    std::cout << t;
    log << t;
    if (nl) {
        std::cout << std::endl;
        log << std::endl;
    } else {
        std::cout << std::flush;
        log << std::flush;
    }
}

/*----------------------------------------------
 *  Image File
 * ---------------------------------------------*/
std::string getAlignResults::getImgFilename(size_t img_i)
{
    char buf[18];
    sprintf(buf, (settings.kfRGBNamePattern).c_str(), img_i);
    std::string name = buf;
    return name;
}
std::string getAlignResults::getImgFilename(size_t img_i, std::string pre, std::string ext)
{
    char filename[18] = "\n";
    sprintf(filename, (pre + "%03d" + ext).c_str(), img_i);
    std::string filename_ = filename;
    return filename_;
}

/*----------------------------------------------
 *  Camera
 * ---------------------------------------------*/
// the cameraPoses are matrixs that project a point from world coord to camera coord
void getAlignResults::readCameraTraj(std::string camTraj_file)
{
    std::ifstream  matifs(camTraj_file.c_str());
    int id;
    while( !matifs.eof() )
    {
        cv::Mat1f mat( cv::Size(4, 4) );
        matifs >> id >> id >> id;
        matifs >> mat.at<float>(0,0) >> mat.at<float>(0,1) >> mat.at<float>(0,2) >> mat.at<float>(0,3);
        matifs >> mat.at<float>(1,0) >> mat.at<float>(1,1) >> mat.at<float>(1,2) >> mat.at<float>(1,3);
        matifs >> mat.at<float>(2,0) >> mat.at<float>(2,1) >> mat.at<float>(2,2) >> mat.at<float>(2,3);
        matifs >> mat.at<float>(3,0) >> mat.at<float>(3,1) >> mat.at<float>(3,2) >> mat.at<float>(3,3);
        if(matifs.fail())
            break;
        if ( ! settings.camTrajFromWorldToCam )
            mat = mat.inv();
        cameraPoses.push_back( mat );
    }
    matifs.close();
    /*for ( size_t i = 0; i < cameraPoses.size(); i++)
        std::cout << cameraPoses[i] << std::endl; */
}
void getAlignResults::readCameraTraj()
{
    char buf[18];
    std::ifstream matifs;
    for( size_t i = 0; i < kfTotal; i++ ){
        sprintf(buf, (settings.camTrajNamePattern).c_str(), i);
        std::string name(buf);
        matifs.open( settings.keyFramesPath + "/" + name );
        cv::Mat1f mat( cv::Size(4, 4) );
        // the first, second, third number are T for camera, others are R for camera
        matifs >> mat.at<float>(0,3) >> mat.at<float>(1,3) >> mat.at<float>(2,3);
        matifs >> mat.at<float>(0,0) >> mat.at<float>(0,1) >> mat.at<float>(0,2);
        matifs >> mat.at<float>(1,0) >> mat.at<float>(1,1) >> mat.at<float>(1,2);
        matifs >> mat.at<float>(2,0) >> mat.at<float>(2,1) >> mat.at<float>(2,2);
        mat.at<float>(3,0) = 0;
        mat.at<float>(3,1) = 0;
        mat.at<float>(3,2) = 0;
        mat.at<float>(3,3) = 1;
        if ( ! settings.camTrajFromWorldToCam )
            mat = mat.inv();
        cameraPoses.push_back(mat);
        matifs.close();
    }
    /*for ( size_t i = 0; i < cameraPoses.size(); i++)
        std::cout << cameraPoses[i] << std::endl; */
}

// project the point to the (id)th camera's coordinate system
cv::Mat getAlignResults::projectToCamera(cv::Mat X_w, size_t id)
{
    cv::Mat R = cameraPoses[id]; // from world to camera
    return R * X_w;
}

// project the point to the (id)th image's plane (on origin-resolution)
//   X_w is the point's world position [x_w, y_w, z_w, 1]
//   return 3*1 matrix [x_img, y_img, z_c]
cv::Mat getAlignResults::projectToImg(cv::Mat X_w, size_t id)
{
    cv::Mat X_c = projectToCamera(X_w, id);
    cv::Mat1f X_img = (cv::Mat_<float>(3, 1) << X_c.at<float>(0), X_c.at<float>(1), X_c.at<float>(2));
    X_img = settings.cameraK * X_img / X_c.at<float>(2);
    X_img.at<float>(2) = X_c.at<float>(2); // store the depth to do the visibility-check
    return X_img;
}

bool getAlignResults::pointValid(cv::Point2i p_img)
{
    if(p_img.x < 0 || p_img.x >= settings.originImgW)
        return false;
    if(p_img.y < 0 || p_img.y >= settings.originImgH)
        return false;
    return true;
}
bool getAlignResults::pointValid(cv::Point2f p_img)
{
    pointValid( cv::Point2i( std::round(p_img.x), std::round(p_img.y)) );
}

// apply the scale to the point p (p is on the origin-resolution img)
cv::Point2i getAlignResults::imgToScale(cv::Point2i p_img)
{
    cv::Point2i p_img_s(0,0);
    p_img_s.x = std::round( p_img.x / scaleF );
    p_img_s.y = std::round( p_img.y / scaleF );
    return p_img_s;
}

// project the position on the scaled img to the origin-resolution img
cv::Point2i getAlignResults::scaleToImg(cv::Point2i p_img_s)
{
    cv::Point2i p_img(0,0);
    p_img.x = std::round( p_img_s.x * scaleF );
    p_img.y = std::round( p_img_s.y * scaleF );
    return p_img;
}

/*----------------------------------------------
 *  Remapping
 * ---------------------------------------------*/
void getAlignResults::calcVertexMapping()
{
    // read the camera's world positions of keyframes
    LOG("[ Read Camera Matrixs ] ");
    if ( EAGLE::isFileExist(settings.keyFramesPath + "/" + settings.kfCameraTxtFile) )
        readCameraTraj(settings.keyFramesPath + "/" + settings.kfCameraTxtFile);
    else
        readCameraTraj();

    // create a RGB point cloud
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_rgb(new pcl::PointCloud<pcl::PointXYZRGB>);
    // convert to PointCloud
    pcl::fromPCLPointCloud2(mesh.cloud, *cloud_rgb);
    size_t point_num = cloud_rgb->points.size();

    // for each vertex, calculate its normal
    std::vector<cv::Vec3f> vertex_normal(point_num); // vertex id => cv::Vec3f
    LOG("[ Calculating Normals of each Vertex ]");
    // for each vertex, calculate its normal
    // ( source: https://blog.csdn.net/wolfcsharp/article/details/93711068 )
    pcl::NormalEstimation<pcl::PointXYZRGB, pcl::Normal> n;
    pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
    //  using kdtree to search NN
    pcl::search::KdTree<pcl::PointXYZRGB>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZRGB>);
    tree->setInputCloud(cloud_rgb);
    n.setInputCloud(cloud_rgb);
    n.setSearchMethod(tree);
    //  set the NN value
    n.setKSearch(20);
    n.compute(*normals);
    // output the normals
#pragma omp parallel for
    for ( size_t i = 0; i < cloud_rgb->points.size(); i++)
        vertex_normal[i] = cv::Vec3f(normals->points[i].normal_x, normals->points[i].normal_y, normals->points[i].normal_z);

    // for each vertex, calculate its uv coordinate on each source image
    LOG("[ Calculating UVs of each Vertex on every Si ]");
    //std::map<size_t, std::vector<cv::Point3f>> uvs;
    for( size_t t : kfIndexs )
        uvs[t] = std::vector<cv::Point3f>(point_num); // vertex id => cv::Point3f
#pragma omp parallel for
    for (size_t i = 0; i < point_num; i++) {
        cv::Mat X_w = (cv::Mat_<float>(4, 1) << cloud_rgb->points[i].x, cloud_rgb->points[i].y, cloud_rgb->points[i].z, 1);
        for( size_t t : kfIndexs ) {
            // get the vertex's position on S(t) at origin resolution
            cv::Mat X_img = projectToImg(X_w, t);
            uvs[t][i] = cv::Point3f(X_img.at<float>(0), X_img.at<float>(1), X_img.at<float>(2));
        }
    }

    // calculate valid mesh for each pixel on every image
    LOG("[ Calculating valid mesh of every pixel on each Si ]");
    //std::map<size_t, cv::Mat> img_valid_mesh;
    img_valid_mesh.clear();
    img_valid_mesh_lamda.clear();
    LOG( " Valid Mesh << ", false );
    for( size_t t : kfIndexs ) {
        img_valid_mesh[t] = cv::Mat1i( cv::Size(settings.originImgW, settings.originImgH) );
        img_valid_mesh_lamda[t] = cv::Mat3f( cv::Size(settings.originImgW, settings.originImgH) );
        for( int i = 0; i < settings.originImgW; i++ )
            for( int j = 0; j < settings.originImgH; j++ ) {
                img_valid_mesh[t].at<int>(j, i) = -1;
                img_valid_mesh_lamda[t].at<cv::Vec3f>(j, i) = cv::Vec3f(-1, -1, -1);
            }
        calcImgValidMesh(t);
        LOG( std::to_string(t) + " ", false );
    }
    LOG( "<< Done" );

    // for each vertex, calculate its weight of each image
    LOG("[ Calculating Weight of each Vertex ]");
    std::map<size_t, std::vector<float>> vertex_weight;
    for( size_t t : kfIndexs )
        vertex_weight[t] = std::vector<float>(point_num);
#pragma omp parallel for
    for(size_t i = 0; i < point_num; i++) {
        cv::Mat X_w = (cv::Mat_<float>(4, 1) << cloud_rgb->points[i].x, cloud_rgb->points[i].y, cloud_rgb->points[i].z, 1);
        cv::Mat D_i = (cv::Mat_<float>(4, 1) << 0,0,1,0);
        cv::Vec3f V_N = vertex_normal[i];
        for( size_t t : kfIndexs ) {
            // get the camera's direction vector
            cv::Mat D = projectToCamera(D_i, t);
            cv::Vec3f V_D = cv::Vec3f(D.at<float>(0), D.at<float>(1), D.at<float>(2));
            float d12  = V_N(0)*V_D(0)+V_N(1)*V_D(1)+V_N(2)*V_D(2);
            float d1_2 = V_N(0)*V_N(0)+V_N(1)*V_N(1)+V_N(2)*V_N(2);
            float d2_2 = V_D(0)*V_D(0)+V_D(1)*V_D(1)+V_D(2)*V_D(2);
            float cos_theta2 = d12 * d12 / (d1_2 * d2_2);
            // get the vertex's relative position
            cv::Mat X_c = projectToCamera(X_w, t);
            float d2 = X_c.at<float>(0) * X_c.at<float>(0) + X_c.at<float>(1) * X_c.at<float>(1) + X_c.at<float>(2) * X_c.at<float>(2);
            vertex_weight[t][i] = cos_theta2 / d2;
        }
    }

    // interpolate vertex's weight to every pixel
    LOG("[ Calculating weight of every pixel on each Si ]");
    //std::map<size_t, cv::Mat> weights;
    weights.clear();
    LOG( " Weights << ", false );
    for( size_t t : kfIndexs ) {
        weights[t] = cv::Mat1f( cv::Size(settings.originImgW, settings.originImgH) );
        for( int i = 0; i < settings.originImgW; i++ )
            for( int j = 0; j < settings.originImgH; j++ )
                weights[t].at<float>(j, i) = 0;
        calcImgWeight(t, vertex_weight[t]);
        LOG( std::to_string(t) + " ", false );
    }
    LOG( "<< Done" );

    // do remapping
    //std::map<size_t, std::map<size_t, cv::Mat>> mappings;
    mappings.clear();
    // for every triangle mesh, do projection from i to j
    LOG("[ Image Remapping ]");
    for( size_t img_i : kfIndexs) {
        mappings[img_i] = std::map<size_t, cv::Mat>();
        LOG( " " + std::to_string(img_i) + " to ", false );
        for( size_t img_j : kfIndexs ) {
            mappings[img_i][img_j] = cv::Mat3i( cv::Size(settings.originImgW, settings.originImgH) );
            for( int i = 0; i < settings.originImgW; i++ )
                for( int j = 0; j < settings.originImgH; j++ )
                    mappings[img_i][img_j].at<cv::Vec3i>(j, i) = cv::Vec3i(0,0,0);
            calcRemapping(img_i, img_j);
            LOG( std::to_string(img_j) + " ", false );
        }
        LOG( "<< Done" );
    }
}

// Barycentric coordinate system
//   https://blog.csdn.net/silangquan/article/details/21990713
//   https://www.zhihu.com/question/38356223
cv::Mat3f getAlignResults::calcPosCoord(cv::Point3f uv1, cv::Point3f uv2, cv::Point3f uv3, cv::Rect &pos)
{
    float x1 = uv1.x, y1 = uv1.y, x2 = uv2.x, y2 = uv2.y, x3 = uv3.x, y3 = uv3.y;
    int max_x = std::ceil( EAGLE_MAX(EAGLE_MAX(x1,x2),x3) );
    int min_x = std::floor( EAGLE_MIN(EAGLE_MIN(x1,x2),x3) );
    int max_y = std::ceil( EAGLE_MAX(EAGLE_MAX(y1,y2),y3) );
    int min_y = std::floor( EAGLE_MIN(EAGLE_MIN(y1,y2),y3) );

    //cv::Rect pos(min_x, min_y, max_x-min_x+1, max_y-min_y+1);
    pos.x = min_x;
    pos.y = min_y;
    pos.width = max_x-min_x+1;
    pos.height = max_y-min_y+1;

    float detT = (x1 - x3) * (y2 - y3) - (x2 - x3) * (y1 - y3);

    cv::Mat3f lamdas( cv::Size(pos.width, pos.height) );
    int total = pos.width * pos.height;
    for ( int index = 0; index < total; index++ ) {
        int dy = index / pos.width;
        int dx = index % pos.width;
        int x = pos.x + dx, y = pos.y + dy;
        lamdas.at<cv::Vec3f>(dy, dx)(0) = ((y2-y3)*(x-x3) + (x3-x2)*(y-y3)) / detT;
        lamdas.at<cv::Vec3f>(dy, dx)(1) = ((y3-y1)*(x-x3) + (x1-x3)*(y-y3)) / detT;
        lamdas.at<cv::Vec3f>(dy, dx)(2) = 1 - lamdas.at<cv::Vec3f>(dy, dx)(0) - lamdas.at<cv::Vec3f>(dy, dx)(1);
    }
    return lamdas;
}

// for each pixel, find the mesh from which it gets the color
void getAlignResults::calcImgValidMesh(size_t img_i)
{
    cv::Mat1i img_valid_z = cv::Mat1i ( cv::Size(settings.originImgW, settings.originImgH) );
    for( int i = 0; i < settings.originImgW; i++ )
        for( int j = 0; j < settings.originImgH; j++ )
            img_valid_z.at<int>(j, i) = -1;
    for( size_t i = 0; i < mesh.polygons.size(); i++ ) {
        size_t p1 = mesh.polygons[i].vertices[0];
        size_t p2 = mesh.polygons[i].vertices[1];
        size_t p3 = mesh.polygons[i].vertices[2];
        cv::Point3f i_uv1 = uvs[img_i][p1];
        cv::Point3f i_uv2 = uvs[img_i][p2];
        cv::Point3f i_uv3 = uvs[img_i][p3];

        cv::Rect pos(0,0,0,0);
        cv::Mat3f lamdas = calcPosCoord(i_uv1, i_uv2, i_uv3, pos);
        int total = pos.width * pos.height;
        for ( int index = 0; index < total; index++ ) {
            int dy = index / pos.width;
            int dx = index % pos.width;
            cv::Point2i p_img_i( pos.x + dx, pos.y + dy );
            if ( ! pointValid(p_img_i) )
                continue;
            cv::Vec3f lamda = lamdas.at<cv::Vec3f>(dy, dx);
            if ( lamda(0) >= 0 && lamda(1) >= 0 && lamda(2) >= 0 ) {
                int z_new = std::round( 10000.0 / (lamda(0)/i_uv1.z + lamda(1)/i_uv2.z + lamda(2)/i_uv3.z) );
                int z_old = img_valid_z.at<int>(p_img_i.y, p_img_i.x);
                if ( z_old < 0 || z_new < z_old ) {
                    img_valid_z.at<int>(p_img_i.y, p_img_i.x) = z_new;
                    img_valid_mesh[img_i].at<int>(p_img_i.y, p_img_i.x) = i;
                    img_valid_mesh_lamda[img_i].at<cv::Vec3f>(p_img_i.y, p_img_i.x) = lamda;
                }
            }
        }
    }
//    std::cout << img_valid_z << std::endl;
}

void getAlignResults::calcImgWeight(size_t img_i, std::vector<float> vertex_weight)
{
#pragma omp parallel for
    for( size_t i = 0; i < mesh.polygons.size(); i++ ) {
        size_t p1 = mesh.polygons[i].vertices[0];
        size_t p2 = mesh.polygons[i].vertices[1];
        size_t p3 = mesh.polygons[i].vertices[2];

        cv::Point3f i_uv1 = uvs[img_i][p1];
        cv::Point3f i_uv2 = uvs[img_i][p2];
        cv::Point3f i_uv3 = uvs[img_i][p3];

        float w1 = vertex_weight[p1];
        float w2 = vertex_weight[p2];
        float w3 = vertex_weight[p3];

        cv::Rect pos(0,0,0,0);
        cv::Mat3f lamdas = calcPosCoord(i_uv1, i_uv2, i_uv3, pos);
        int total = pos.width * pos.height;
        int dx, dy; float w;
        for ( int index = 0; index < total; index++ ) {
            dy = index / pos.width;
            dx = index % pos.width;
            cv::Point2i p_img( pos.x + dx, pos.y + dy );
            if( ! pointValid(p_img) )
                continue;
            if( img_valid_mesh[img_i].at<int>(p_img.y, p_img.x) != i )
                continue;
            cv::Vec3f lamda = lamdas.at<cv::Vec3f>(dy, dx);
            if( lamda(0) >= 0 && lamda(1) >= 0 && lamda(2) >= 0 ) {
                w = w1*lamda(0) + w2*lamda(1) + w3*lamda(2);
                weights[img_i].at<float>(p_img.y, p_img.x) = w;
            }
        }
    }
}

// for each pixel in img_i, remapping it to img_j only when the mesh is visible both in i and j
void getAlignResults::calcRemapping(size_t img_i, size_t img_j)
{
    int total = settings.imgH * settings.imgW;
#pragma omp parallel for
    for ( int pixel_index = 0; pixel_index < total; pixel_index++) {
        int y = pixel_index / settings.imgW;
        int x = pixel_index % settings.imgW;
        int i = img_valid_mesh[img_i].at<int>(y, x);
        if( i < 0 )
            continue;

        if( img_i == img_j ){
            mappings[img_i][img_j].at<cv::Vec3i>(y, x)(0) = x;
            mappings[img_i][img_j].at<cv::Vec3i>(y, x)(1) = y;
            mappings[img_i][img_j].at<cv::Vec3i>(y, x)(2) = 1;
            continue;
        }

        cv::Vec3f lamda = img_valid_mesh_lamda[img_i].at<cv::Vec3f>(y, x);
        size_t p1 = mesh.polygons[i].vertices[0];
        size_t p2 = mesh.polygons[i].vertices[1];
        size_t p3 = mesh.polygons[i].vertices[2];
        cv::Point3f j_uv1 = uvs[img_j][p1];
        cv::Point3f j_uv2 = uvs[img_j][p2];
        cv::Point3f j_uv3 = uvs[img_j][p3];

        cv::Point2i p_img_j;
        p_img_j.x = std::round( j_uv1.x * lamda(0) + j_uv2.x * lamda(1) + j_uv3.x * lamda(2) );
        p_img_j.y = std::round( j_uv1.y * lamda(0) + j_uv2.y * lamda(1) + j_uv3.y * lamda(2) );
        if ( pointValid( p_img_j ) && img_valid_mesh[img_j].at<int>(p_img_j.y, p_img_j.x) == i ) {
            mappings[img_i][img_j].at<cv::Vec3i>(y, x)(0) = p_img_j.x;
            mappings[img_i][img_j].at<cv::Vec3i>(y, x)(1) = p_img_j.y;
            mappings[img_i][img_j].at<cv::Vec3i>(y, x)(2) = 1;
        }
    }
}

/*----------------------------------------------
 *  Do Iterations
 * ---------------------------------------------*/
void getAlignResults::doIterations()
{
    int scale = 0;
//    scale = settings.scaleTimes-1;
    int iter_count = 50;
    std::vector<cv::String> originSourcesFiles(sourcesFiles); // origin sources
    for ( ; scale < settings.scaleTimes; scale++) {
        // downsample imgs
        settings.imgW = std::round(settings.scaleInitW * pow(settings.scaleFactor, scale));
        settings.imgH = std::round(settings.scaleInitH * pow(settings.scaleFactor, scale));
        scaleF = pow(settings.scaleFactor, settings.scaleTimes-1-scale);

        char tmp[10];
        sprintf(tmp, "%dx%d", settings.imgW, settings.imgH);
        std::string newResolution(tmp);
        LOG("[ Scale to " + newResolution + " ]");

        // get all keyframes' full path (after scale)
        sourcesPath = settings.keyFramesPath + "/" + newResolution;
        EAGLE::checkPath(sourcesPath);
        // generate source imgs with new resolution // [REQUIRE] ImageMagick
        for( size_t i = 0; i < kfTotal; i++ )
            system( ("convert " + originSourcesFiles[i] + " -resize " + newResolution + "! " + sourcesPath+"/" +getImgFilename(i)).c_str() );
        cv::glob(sourcesPath + "/" + settings.kfRGBMatch, sourcesFiles, false);
        // read Si
        sourcesImgs.clear();
        for ( size_t i = 0; i < kfTotal; i++ )
            sourcesImgs.push_back( cv::imread(sourcesFiles[i]) ); // img.at<cv::Vec3b>(y, x)(0)

        // init Ti and Mi or upsample
        if ( targetsFiles.size() == 0 ) {
            for( size_t i = 0; i < kfTotal; i++ ) {
                system( ("cp " + sourcesFiles[i] + " " + targetsPath+"/").c_str() );
                system( ("cp " + sourcesFiles[i] + " " + texturesPath+"/").c_str() );
            }
            cv::glob(targetsPath + "/" + settings.kfRGBMatch, targetsFiles, false);
            cv::glob(texturesPath + "/" + settings.kfRGBMatch, texturesFiles, false);
        }else{
            for( size_t i : kfIndexs ){
                // [REQUIRE] ImageMagick
                system( ("convert " + targetsFiles[i] + " -resize " + newResolution + "! " + targetsFiles[i]).c_str() );
                system( ("convert " + texturesFiles[i] + " -resize " + newResolution + "! " + texturesFiles[i]).c_str() );
            }
        }

        if( ! OUTPUT_T_M_INSTANT) {
            targetsImgs.clear();
            texturesImgs.clear();
            for( size_t i = 0; i < kfTotal; i++ ) {
                targetsImgs.push_back( cv::imread(targetsFiles[i]) );
                texturesImgs.push_back( cv::imread(texturesFiles[i]) );
            }
        }
        // do iterations
        for ( int _count = 0; _count < iter_count; _count++) {
            LOG("[ Iteration " + std::to_string(_count+1) + " at " + newResolution + " ]");
            calcPatchmatch();
            generateTargets();
            generateTextures();
        }
        iter_count -= 5;
        if( ! OUTPUT_T_M_INSTANT) {
            for( size_t i : kfIndexs ){
                cv::imwrite( targetsFiles[i], targetsImgs[i] );
                cv::imwrite( texturesFiles[i], texturesImgs[i] );
            }
        }

        // save results
        std::string texturesResultPath = resultsPath + "/" + newResolution;
        EAGLE::checkPath(texturesResultPath);
        for( size_t i : kfIndexs ){
            system( ("cp " + texturesFiles[i] + " " + texturesResultPath+"/" + getImgFilename(i, "M_", "."+settings.rgbNameExt)).c_str() );
            system( ("cp " + targetsFiles[i] + " " + texturesResultPath+"/" + getImgFilename(i, "T_", "."+settings.rgbNameExt)).c_str() );
        }
        LOG( "[ Results at " + newResolution + " Saving Success ]" );
    }
}

/*----------------------------------------------
 *  PatchMatch
 * ---------------------------------------------*/
void getAlignResults::calcPatchmatch()
{
    if(LOG_PM)
        LOG( " Patchmatchs << ", false );
    std::string source_file, target_file, ann_file, annd_file;
    for (size_t i : kfIndexs) {
        source_file = sourcesFiles[i];
        target_file = targetsFiles[i];

        ann_file = pmResultPath + "/" + getAnnFilename(i, "s2t.jpg");
        annd_file = pmResultPath + "/" + getAnndFilename(i, "s2t.jpg");
        patchMatch(source_file, target_file, ann_file, annd_file);

        ann_file = pmResultPath + "/" + getAnnFilename(i, "t2s.jpg");
        annd_file = pmResultPath + "/" + getAnndFilename(i, "t2s.jpg");
        patchMatch(target_file, source_file, ann_file, annd_file);

        if(LOG_PM)
            LOG( std::to_string(i) + " ", false);
    }
    if(LOG_PM)
        LOG( "<< Done" );
}
void getAlignResults::patchMatch(std::string imgA_file, std::string imgB_file, std::string ann_file, std::string annd_file)
{
    // bin imageA_filename imageB_filename ann_img_filename annd_img_filename
    std::string buf = settings.patchmatchBinFile + " " + imgA_file + " " + imgB_file + " " + ann_file + " " + annd_file;
    system( buf.c_str() );
}
// img_file = "/home/wsy/EAGLE/00000.jpg"
// sym = "s2t.jpg"
//  return "/home/wsy/EAGLE/00000_s2t.jpg"
std::string getAlignResults::getAnnFilename(size_t img_id, std::string sym)
{
    return std::to_string(img_id) + "_ann_" + sym;
}
// img_file = "/home/wsy/EAGLE/00001.jpg"
// sym = "t2s.jpg"
//  return "/home/wsy/EAGLE/00001_t2s.jpg"
std::string getAlignResults::getAnndFilename(size_t img_id, std::string sym)
{
    return std::to_string(img_id) + "_annd_" + sym;
}
// ann_txt_file = "/home/wsy/EAGLE/00000_s2t.txt"
// cv::Mat1i result(480, 640) // for 640X480 img to store the ANN result of PatchMatch
void getAlignResults::readAnnTXT(std::string ann_txt_file, cv::Mat1i &result)
{
    std::ifstream infile(ann_txt_file.c_str(), std::ios::in | std::ios::binary);
    if ( !infile.is_open() ) {
      LOG("!!! Open file [" + ann_txt_file + "] failed!");
      return;
    }
    for( int j = 0; j < result.size().height; j++ ){
        for( int i = 0; i < result.size().width; i++ ){
            infile >> result.at<int>(j, i);
        }
    }
    infile.close();
}
double getAlignResults::calcAnndSum(std::string annd_txt_file)
{
    std::ifstream infile(annd_txt_file.c_str(), std::ios::in | std::ios::binary);
    if ( !infile.is_open() ) {
      LOG("!!! Open file [" + annd_txt_file + "] failed !!!");
      return 0;
    }
    double result = 0;
    int v = 0;
    while( ! infile.eof() ) {
        infile >> v;
        result += (v * 1.0 / settings.patchSize);
    }
    infile.close();
    return result;
}

/*----------------------------------------------
 *  Generate Tis
 * ---------------------------------------------*/
void getAlignResults::generateTargets()
{
    E1 = 0;
    if(LOG_SAVE_T)
        LOG( " Targets << ", false );
    if(OUTPUT_T_M_INSTANT){
        texturesImgs.clear();
        for( size_t i = 0; i < kfTotal; i++ )
            texturesImgs.push_back( cv::imread(texturesFiles[i]) );
    }
    for( size_t i : kfIndexs ) {
        generateTargetI(i, texturesImgs);
        if(LOG_SAVE_T)
            LOG( std::to_string(i) + " ", false);
    }
    if(LOG_SAVE_T)
        LOG( "<< Done << E1: " + std::to_string(E1) );
}
void getAlignResults::generateTargetI(size_t target_id, std::vector<cv::Mat3b> textures)
{
    int total = settings.imgH * settings.imgW;
    int totalPatch = (settings.imgW - settings.patchWidth + 1) * (settings.imgH - settings.patchWidth + 1);
    cv::Mat3b target( cv::Size(settings.imgW, settings.imgH) );

    // similarity term
    cv::Mat1i result_ann_s2t( settings.imgH, settings.imgW );
    readAnnTXT( pmResultPath + "/" + getAnnFilename(target_id, "s2t.txt"), result_ann_s2t);
    cv::Mat1i result_ann_t2s( settings.imgH, settings.imgW );
    readAnnTXT( pmResultPath + "/" + getAnnFilename(target_id, "t2s.txt"), result_ann_t2s);
    cv::Mat4i result_su( cv::Size(settings.imgW, settings.imgH) );
    cv::Mat4i result_sv( cv::Size(settings.imgW, settings.imgH) );
#pragma omp parallel for
    for ( int index = 0; index < total; index++) {
        int j = index / settings.imgW;
        int i = index % settings.imgW;
        result_su.at<cv::Vec4i>(j, i) = cv::Vec4i(0,0,0,0);
        result_sv.at<cv::Vec4i>(j, i) = cv::Vec4i(0,0,0,0);
    }
    getSimilarityTerm(sourcesImgs[target_id], result_ann_s2t, result_ann_t2s, result_su, result_sv);

    // calculate E1
    double E1_1 = calcAnndSum( pmResultPath + "/" + getAnndFilename(target_id, "s2t.txt") );
    double E1_2 = calcAnndSum( pmResultPath + "/" + getAnndFilename(target_id, "t2s.txt") );
    E1 += (settings.alpha_u * E1_1 + settings.alpha_v * E1_2);

#pragma omp parallel for
    for ( int index = 0; index < total; index++) {
        int j = index / settings.imgW;
        int i = index % settings.imgW;
        cv::Point2i p_img = scaleToImg( cv::Point2i(i, j) );
        double weight = weights[target_id].at<float>(p_img.y, p_img.x);
        double _factor1, _factor2; cv::Vec3d sum_bgr(0,0,0);

        // similarity term
        cv::Vec3d sum_S(0,0,0);
        _factor1  = settings.alpha_u * result_su.at<cv::Vec4i>(j,i)(3) / settings.patchSize;
        _factor1 += settings.alpha_v * result_sv.at<cv::Vec4i>(j,i)(3) / settings.patchSize;
        for( int p_i = 0; p_i < 3; p_i++ ) {
            sum_S(p_i) += settings.alpha_u * result_su.at<cv::Vec4i>(j,i)(p_i) / settings.patchSize;
            sum_S(p_i) += settings.alpha_v * result_sv.at<cv::Vec4i>(j,i)(p_i) / settings.patchSize;
            sum_bgr(p_i) = sum_S(p_i);
        }

        // consistency term
        cv::Vec3i sum_M(0,0,0); int count_M = 0;
        _factor2 = settings.lamda * weight;
        // if the pixel is in bg, then no optimization with consistency term
        cv::Vec3i Xij = mappings[target_id][target_id].at<cv::Vec3i>(p_img.y, p_img.x);
        if ( Xij(2) > 0 ) {
            for( size_t t : kfIndexs ) {
                if ( t == target_id ) {
                    sum_M += textures[t].at<cv::Vec3b>(j, i);
                    count_M += 1;
                } else {
                    cv::Vec3i Xij = mappings[target_id][t].at<cv::Vec3i>(p_img.y, p_img.x);
                    if ( Xij(2) > 0 ){
                        cv::Point2i p_img_t(cv::Point2i(Xij(0), Xij(1)));
                        cv::Point2i p_img_ts = imgToScale(p_img_t);
                        sum_M += textures[t].at<cv::Vec3b>(p_img_ts.y, p_img_ts.x);
                        count_M += 1;
                    }
                }
            }
            for ( int p_i = 0; p_i < 3; p_i++ )
              sum_bgr(p_i) += _factor2 * sum_M(p_i) / count_M;
        }

        // generate the pixel of Ti
        cv::Vec3b bgr(0,0,0);
        for ( int p_i = 0; p_i < 3; p_i++ ) {
            if ( count_M > 0 ) {
                sum_bgr(p_i) = sum_bgr(p_i) / (_factor1 + _factor2);
            } else {
                sum_bgr(p_i) = sum_bgr(p_i) / (_factor1);
            }
            bgr(p_i) = EAGLE_MAX( EAGLE_MIN(std::round(sum_bgr(p_i)), 255), 0 );
        }
        target.at<cv::Vec3b>(j, i) = bgr;
    }
    if(OUTPUT_T_M_INSTANT) {
        cv::imwrite( targetsFiles[target_id], target );
    } else {
        targetsImgs[target_id] = target;
    }
}

void getAlignResults::getSimilarityTerm(cv::Mat3b S, cv::Mat1i ann_s2t, cv::Mat1i ann_t2s, cv::Mat4i &su, cv::Mat4i &sv)
{
    int total = settings.imgH * settings.imgW;
#pragma omp parallel for
    for ( int index = 0; index < total; index++) {
        int j = index / settings.imgW;
        int i = index % settings.imgW;
        int x, y, v;
        // Su: completeness
        // here, (i,j) is on Si, and (x,y) on Ti
        if( i >= settings.imgW - (settings.patchWidth-1) || j >= settings.imgH - (settings.patchWidth-1)) {
            calcSuv(S, i, j, su, i, j, 1);
        } else {
            v = ann_s2t.at<int>(j, i);
            x = INT_TO_X(v); y = INT_TO_Y(v);
            calcSuv(S, i, j, su, x, y, settings.patchWidth);
        }
        // Sv: coherence
        // here, (i,j) is on Ti, and (x,y) on Si
        if( i >= settings.imgW - (settings.patchWidth-1) || j >= settings.imgH - (settings.patchWidth-1)) {
            calcSuv(S, i, j, sv, i, j, 1);
        } else {
            v = ann_t2s.at<int>(j, i);
            x = INT_TO_X(v); y = INT_TO_Y(v);
            calcSuv(S, x, y, sv, i, j, settings.patchWidth);
        }
    }
}

void getAlignResults::calcSuv(cv::Mat3b S, int i, int j, cv::Mat4i &s, int x, int y, int w)
{
    for ( int dy = 0; dy < w; dy++ ) {
        for ( int dx = 0; dx < w; dx++ ) {
            if( ! pointValid( cv::Point2i(x+dx, y+dy) ) || ! pointValid( cv::Point2i(i+dx, j+dy) ) )
                continue;
            s.at<cv::Vec4i>(y + dy, x + dx)(0) += S.at<cv::Vec3b>(j + dy, i + dx)(0);
            s.at<cv::Vec4i>(y + dy, x + dx)(1) += S.at<cv::Vec3b>(j + dy, i + dx)(1);
            s.at<cv::Vec4i>(y + dy, x + dx)(2) += S.at<cv::Vec3b>(j + dy, i + dx)(2);
            s.at<cv::Vec4i>(y + dy, x + dx)(3) += 1;
        }
    }
}

/*----------------------------------------------
 *  Generate Mis
 * ---------------------------------------------*/
void getAlignResults::generateTextures()
{
    E2 = 0;
    if(LOG_SAVE_M)
        LOG( " Textures << ", false );
    if(OUTPUT_T_M_INSTANT) {
        targetsImgs.clear();
        for( size_t i = 0; i < kfTotal; i++ )
           targetsImgs.push_back( cv::imread(targetsFiles[i]) );
    }
    for( size_t i : kfIndexs ) {
        generateTextureI(i, targetsImgs);
        if(LOG_SAVE_M)
            LOG( std::to_string(i) + " ", false);
    }
    if(LOG_SAVE_M)
        LOG("<< Done << E2: " + std::to_string(E2));
}
void getAlignResults::generateTextureI(size_t texture_id, std::vector<cv::Mat3b> targets)
{
    int total = settings.imgH * settings.imgW;
    cv::Mat3b texture( cv::Size(settings.imgW, settings.imgH) ); // texturesImgs[texture_id]
#pragma omp parallel for
    for ( int index = 0; index < total; index++) {
        int j = index / settings.imgW;
        int i = index % settings.imgW;
        cv::Point2i p_img = scaleToImg( cv::Point2i(i, j) );

        cv::Vec3b pixel; bool flag_valid;
        float weight = 0, sum_r = 0, sum_g = 0, sum_b = 0, sum_w = 0;

        // for E2 calculation
        std::vector<cv::Vec3b> E2_pixels;
        std::vector<float> E2_weights;

        for ( size_t t : kfIndexs ) {
            flag_valid = true;
            if ( t == texture_id ) {
                weight = weights[t].at<float>(p_img.y, p_img.x);
                pixel = targets[t].at<cv::Vec3b>(j, i);
            } else {
                cv::Vec3i Xij = mappings[texture_id][t].at<cv::Vec3i>(p_img.y, p_img.x);
                if ( Xij(2) > 0 ){
                    cv::Point2i p_img_t = cv::Point2i(Xij(0), Xij(1));
                    weight = weights[t].at<float>(p_img_t.y, p_img_t.x);
                    cv::Point2i p_img_ts = imgToScale(p_img_t);
                    pixel = targets[t].at<cv::Vec3b>(p_img_ts.y, p_img_ts.x);
                } else
                    flag_valid = false;
            }
            if(flag_valid == true) {
                sum_w += weight;
                sum_b = sum_b + weight * pixel(0);
                sum_g = sum_g + weight * pixel(1);
                sum_r = sum_r + weight * pixel(2);

                E2_pixels.push_back(pixel);
                E2_weights.push_back(weight);
            }
        }
        texture.at<cv::Vec3b>(j, i)(0) = std::round( sum_b / sum_w );
        texture.at<cv::Vec3b>(j, i)(1) = std::round( sum_g / sum_w );
        texture.at<cv::Vec3b>(j, i)(2) = std::round( sum_r / sum_w );

        // calculating E2
        if(E2_pixels.size() > 0) {
            double E2_1 = 0;
            for( size_t _i = 0; _i < E2_pixels.size(); _i++ ) {
                double E2_2 = 0;
                for( int _pi = 0; _pi < 3; _pi++ ) {
                    int p1 = E2_pixels[_i](_pi);
                    int p2 = texture.at<cv::Vec3b>(j,i)(_pi);
                    E2_2 += std::pow(p1-p2, 2);
                }
                E2_1 += (E2_2 * E2_weights[_i]);
            }
            E2 += (E2_1 / E2_pixels.size());
        }
    }
    if(OUTPUT_T_M_INSTANT) {
        cv::imwrite( texturesFiles[texture_id], texture );
    } else {
        texturesImgs[texture_id] = texture;
    }
}

/*----------------------------------------------
 *  Generate PLY
 * ---------------------------------------------*/
void getAlignResults::generateColoredPLY(std::string path, std::string filename, std::vector<cv::Mat3b> imgs)
{
    // create a RGB point cloud
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud_rgb(new pcl::PointCloud<pcl::PointXYZRGB>);
    // convert to PointCloud
    pcl::fromPCLPointCloud2(mesh.cloud, *cloud_rgb);

    for( size_t i = 0; i < mesh.polygons.size(); i++ ) {
        for ( size_t pi = 0; pi < 3; pi++ ){
            size_t p = mesh.polygons[i].vertices[pi];
            cv::Vec3i pixel_sum(0,0,0);
            int count = 0;
            for ( size_t t : kfIndexs ) {
                cv::Point3f pi_uv = uvs[t][p];
                cv::Point2i p_img(std::round(pi_uv.x), std::round(pi_uv.y));
                if ( pointValid(p_img) && img_valid_mesh[t].at<int>(p_img.y, p_img.x) == i ) {
                    cv::Vec3b pixel = imgs[t].at<cv::Vec3b>(p_img.y, p_img.x);
                    pixel_sum += pixel;
                    count += 1;
                }
            }
            if ( count > 0 ) {
                cloud_rgb->points[p].b = pixel_sum(0) * 1.0 / count;
                cloud_rgb->points[p].g = pixel_sum(1) * 1.0 / count;
                cloud_rgb->points[p].r = pixel_sum(2) * 1.0 / count;
            }
        }
    }
    // convert to mesh
    pcl::toPCLPointCloud2(*cloud_rgb, mesh.cloud);
    pcl::io::savePLYFile( path + "/" + filename, mesh );
}
