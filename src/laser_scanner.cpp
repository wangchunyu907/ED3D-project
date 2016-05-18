//#include "stdafx.h"

#include "laser_scanner.h"

#include <osgDB/ReadFile>

#include <osgUtil/Optimizer>
#include <osg/ComputeBoundsVisitor>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <iostream>
#include <vector>

#include <pcl/visualization/pcl_visualizer.h>	

int _waitKey() {

	#ifdef _WIN32
		system("pause");
	#elif linux 
		system("read");
	#endif
		
	return 0; 
}

int main(int argc, char** argv)
{
	/////////////////////////////////////////////////////////////////////////////////////
	
	// lettura dei dati di configurazione
	Configuration confData;
	read_config(confData);
	double deg2rad = 2 * 3.1416 / 360;
	// Caricamento del modello
	std::cout << "Caricamento del modello \"" << confData.modelName << "\"...";
	osg::ref_ptr<osg::Node> model = osgDB::readNodeFile(confData.modelName);
	if (!model)
	{
		std::cout << "Nessun modello trovato con quel nome." << std::endl;
		_waitKey();
		return 1;
	}
	std::cout << " OK" << std::endl;

	// Ottimizzazione della mesh
	osgUtil::Optimizer optimizer;
	optimizer.optimize(model.get());

	// Calcolo delle dimensioni della mesh 
	osg::ComputeBoundsVisitor cbbv;
	model->accept(cbbv);
	osg::BoundingBox bb = cbbv.getBoundingBox();
	osg::Vec3 modelSize = bb._max - bb._min;

	std::cout << "Dimensioni del modello:" << std::endl;
	std::cout << "  x_min: " << bb.xMin() << "\tx_max: " << bb.xMax() << "\t\tx: " << modelSize[0] << "mm" << std::endl;
	std::cout << "  y_min: " << bb.yMin() << "\t\ty_max: " << bb.yMax() << "\t\ty: " << modelSize[1] << "mm" << std::endl;
	std::cout << "  z_min: " << bb.zMin() << "\t\tz_max: " << bb.zMax() << "\t\tz: " << modelSize[2] << "mm" << std::endl;

	//impostazione posizione iniziale della telecamera
	int cameraZ = (int) bb.zMin() + confData.cameraHeight; 
	int cameraX = (int) (bb.xMax() + bb.xMin())/2;


	if (!confData.useBounds) {	//calcolo bound ottimale per scansionare l'intero oggetto
		confData.minY = bb.yMin() - confData.cameraHeight*tan(deg2rad*(90 - confData.alphaLaser)) + confData.laserDistance;
		confData.maxY = bb.yMax() + confData.cameraHeight*tan(deg2rad*(90 - confData.alphaLaser)) - confData.laserDistance;
	}

	//Ottimizzazione apertura del laser
	float central_length = confData.cameraHeight/sin(deg2rad*confData.alphaLaser);
	confData.fanLaser = 2*atan((modelSize[0]/2)/central_length)/deg2rad;
	if (confData.fanLaser > 45) {
		cout << "ATTENZIONE: l'apertura del fascio laser necessaria a coprire l'intero modello\n";
		cout << "^^^^^^^^^^  supera il valore massimo, aumentare l'altezza del sistema telecamera/laser";
		//editconfig
	}

	// Calcolo l'altezza del punto di intersezione dei piani laser. 
	float z_intersection = cameraZ - confData.laserDistance*tan(deg2rad*confData.alphaLaser);
	if (z_intersection <= bb.zMax()) {
		std::cout << "ATTENZIONE: l'altezza dei punti di intersezione tra i due piani laser " << std::endl;
		std::cout << "^^^^^^^^^^  e' inferiore all'altezza massima del modello caricato." << std::endl;
		std::cout << "^^^^^^^^^^  e' consigliato alzare l'altezza del sistema telecamera/laser di almeno " 
					<< ceil(bb.zMax() - z_intersection) << "mm." << std::endl;

		std::cout << "Procedere comunque (s/n)? ";
		char answer;
		while(true) {
			cin >> answer;
			if (answer == 's' || answer == 'S') {
				std::cout << "La scansione viene eseguita ignorando il suggerimento.\n";
				break;
			}
			
			else if (answer == 'n' || answer == 'N') {
					//editconfig
					
					std::cout << "Programma terminato senza eseguire la scansione.\n";
					return 1;
				}
		}
	}
	//////////////////////////////////////////////////////////////
	// Ottenimento piani laser. Nel sistema di riferimento della telecamera, i piani non cambiano mai equazione 
	// e possono quindi essere calcolati solo una volta

	// Tre punti per piano laser
	osg::Vec3 a1(0,	confData.laserDistance,	0 );

	osg::Vec3 a2(0,		
		confData.laserDistance - confData.laserLength*sin(deg2rad*(90 - confData.alphaLaser)),	
		-confData.laserLength*cos(deg2rad*(90 - confData.alphaLaser)));

	osg::Vec3 a3(100,
		confData.laserDistance - confData.laserLength*sin(deg2rad*(90 - confData.alphaLaser)),
		-confData.laserLength*cos(deg2rad*(90 - confData.alphaLaser)));


	osg::Vec3 b1(0,	-confData.laserDistance, 0);

	osg::Vec3 b2(0,
		-confData.laserDistance + confData.laserLength*sin(deg2rad*(90 - confData.alphaLaser)),
		-confData.laserLength*cos(deg2rad*(90 - confData.alphaLaser)));

	osg::Vec3 b3(100,
		confData.laserDistance - confData.laserLength*sin(deg2rad*(90 - confData.alphaLaser)),
		-confData.laserLength*cos(deg2rad*(90 - confData.alphaLaser)));

	osg::Plane planeA(a1, a2, a3);
	osg::Plane planeB(b1, b2, b3);
	
	// Ottenimento dei coefficienti delle equazioni dei piani laser
	osg::Vec4d planeA_coeffs = planeA.asVec4();
	osg::Vec4d planeB_coeffs = planeB.asVec4();
	
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////

	// Fase iterativa. Per ogni frame viene scansionata l'immagine e inseriti i punti nella PC

	// Inizializzazione delle variabili necessarie per la visualizzazione della progressione
	float iter = 0;
	float total_space = confData.maxY - confData.minY;
	float space_between_frame = confData.scanSpeed/confData.fpsCam; // in millimetri
	float num_iterations = total_space / space_between_frame + 1;

	// Inizializzazione point cloud da riempire
	pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);

	for (float cameraY = confData.minY; cameraY <= confData.maxY; cameraY += space_between_frame) {

		cv::Vec3f position(cameraX, cameraY, cameraZ);
		confData.cameraPos = position;
		// Istruzioni per l'aggiornamento dello stato su console
		float progress = ++iter / num_iterations * 100;
		printf("\rScansionando la posizione y=%2.2f. Completamento al %2.2f%% ", cameraY, progress);
		std::cout<<std::flush;

		// Chiamata al metodo per ottenere le catture della macchina fotografica.
		cv::Mat screenshot = reproject(model, confData);

		// Conversione dall'immagine allo spazio 3D
		convert_to_3d(screenshot, confData, planeA_coeffs, planeB_coeffs, cloud);
	
	}

	// Visualizzazione finale della point cloud
	std::cout << "Punti nella cloud " << cloud->points.size() << std::endl;
	pcl::visualization::PCLVisualizer pcl_viewer("PCL Viewer");
	pcl_viewer.addCoordinateSystem(0.1);
	pcl_viewer.addPointCloud<pcl::PointXYZ>(cloud, "input_cloud");
	pcl_viewer.setBackgroundColor(0.2, 0.2, 0.2);
	pcl_viewer.spin();

	cout << "Exit program." << endl;
	//_waitKey();

	return 0;

}
