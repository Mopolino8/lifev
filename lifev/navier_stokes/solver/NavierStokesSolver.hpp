//@HEADER
/*
*******************************************************************************

    Copyright (C) 2004, 2005, 2007 EPFL, Politecnico di Milano, INRIA
    Copyright (C) 2010 EPFL, Politecnico di Milano, Emory University

    This file is part of LifeV.

    LifeV is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    LifeV is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with LifeV.  If not, see <http://www.gnu.org/licenses/>.

*******************************************************************************
*/
//@HEADER

/*!

 */


#ifndef NAVIERSTOKESSOLVERBLOCK_H
#define NAVIERSTOKESSOLVERBLOCK_H 1

#include <lifev/core/LifeV.hpp>

// includes for matrices and vector
#include <lifev/core/array/MatrixEpetra.hpp>
#include <lifev/core/array/VectorEpetra.hpp>

// including data container for the fluid problem
#include <lifev/navier_stokes/solver/OseenData.hpp>

// includes for building the matrix graph
#include <Epetra_FECrsGraph.h>
#include <lifev/eta/expression/Integrate.hpp>
#include <lifev/eta/expression/BuildGraph.hpp>

// classical FE space
#include <lifev/core/fem/FESpace.hpp>

// expression template finite element space
#include <lifev/eta/fem/ETFESpace.hpp>

// utilities
#include <lifev/core/util/LifeChrono.hpp>
#include <lifev/core/util/Displayer.hpp>

namespace LifeV
{

class NavierStokesSolver
{

public:

	typedef RegionMesh<LinearTetra> mesh_Type;
	typedef boost::shared_ptr<mesh_Type> meshPtr_Type;

	typedef MapEpetra map_Type;
	typedef boost::shared_ptr<map_Type> mapPtr_Type;

	typedef MatrixEpetra<Real> matrix_Type;
	typedef boost::shared_ptr<matrix_Type> matrixPtr_Type;

	typedef VectorEpetra vector_Type;
	typedef boost::shared_ptr<vector_Type> vectorPtr_Type;

	typedef GetPot dataFile_Type;
	typedef boost::shared_ptr<dataFile_Type> dataFilePtr_Type;

	typedef Epetra_Comm comm_Type;
	typedef boost::shared_ptr< comm_Type > commPtr_Type;

	typedef Epetra_FECrsGraph graph_Type;
	typedef boost::shared_ptr<Epetra_FECrsGraph> graphPtr_Type;

	typedef ETFESpace<mesh_Type, map_Type, 3, 3 > ETFESpace_velocity;
	typedef ETFESpace<mesh_Type, map_Type, 3, 1 > ETFESpace_pressure;

	// Constructor
	NavierStokesSolver(const dataFile_Type dataFile, const commPtr_Type& communicator);

	// Destructor
	~NavierStokesSolver();

	// Setup
	void setup(const meshPtr_Type& mesh);

	// Assemble constant terms
	void buildSystem();

private:

	// communicator
	commPtr_Type M_comm;

	// getpot object
	dataFile_Type M_dataFile;

	// fluid data
	boost::shared_ptr<OseenData> M_fluidData;

	// FE spaces
	boost::shared_ptr<FESpace<mesh_Type, map_Type> > M_velocityFESpace;
	boost::shared_ptr<FESpace<mesh_Type, map_Type> > M_pressureFESpace;

	// ET FE Spaces
	boost::shared_ptr<ETFESpace_velocity > M_fespaceUETA;
	boost::shared_ptr<ETFESpace_pressure > M_fespacePETA;

	// stiff-strain check
	bool M_stiffStrain;

	// graphs for the matrices
	graphPtr_Type M_Mu_graph;
	graphPtr_Type M_Btranspose_graph;
	graphPtr_Type M_B_graph;
	graphPtr_Type M_C_graph;
	graphPtr_Type M_A_graph;

	// matrices
	matrixPtr_Type M_Mu;
	matrixPtr_Type M_Btranspose;
	matrixPtr_Type M_B;
	matrixPtr_Type M_C;
	matrixPtr_Type M_A;

	// vectors
	vectorPtr_Type M_uExtrapolated;

	//! Displayer to print in parallel (only PID 0 will print)
	Displayer M_displayer;

}; // class NavierStokesSolver


NavierStokesSolver::NavierStokesSolver(const dataFile_Type dataFile, const commPtr_Type& communicator):
		M_comm(communicator),
		M_dataFile(dataFile),
		M_displayer(communicator)
{
}

NavierStokesSolver::~NavierStokesSolver()
{
}


void NavierStokesSolver::setup(const meshPtr_Type& mesh)
{
	M_fluidData.reset( new OseenData() );
	M_fluidData->setup( M_dataFile );

	std::string uOrder = M_dataFile("fluid/space_discretization/vel_order","P1");
	std::string pOrder = M_dataFile("fluid/space_discretization/pres_order","P1");

	Int geoDimensions = mesh_Type::S_geoDimensions;

	M_velocityFESpace.reset (new FESpace<mesh_Type, map_Type> (mesh, uOrder, geoDimensions, M_comm) );
	M_pressureFESpace.reset (new FESpace<mesh_Type, map_Type> (mesh, pOrder, geoDimensions, M_comm) );

	M_fespaceUETA.reset( new ETFESpace_velocity(M_velocityFESpace->mesh(), &(M_velocityFESpace->refFE()), M_comm));
	M_fespacePETA.reset( new ETFESpace_pressure(M_pressureFESpace->mesh(), &(M_pressureFESpace->refFE()), M_comm));

	M_uExtrapolated.reset( new vector_Type ( M_velocityFESpace->map(), Repeated ) );
	*M_uExtrapolated *= 0;

	M_stiffStrain = M_dataFile("fluid/space_discretization/stiff_strain", true);
}

void NavierStokesSolver::buildSystem()
{
	M_displayer.leaderPrint ( " F - Pre-building the graphs... ");
	LifeChrono chrono;
	chrono.start();

	{
		using namespace ExpressionAssembly;

		// Graph velocity mass -> block (0,0)
		M_Mu_graph.reset (new Epetra_FECrsGraph (Copy, * (M_fespaceUETA->map().map (Unique) ), 0) );
		buildGraph ( elements (M_fespaceUETA->mesh() ),
				     quadRuleTetra4pt,
				     M_fespaceUETA,
				     M_fespaceUETA,
				     dot ( phi_i, phi_j )
		) >> M_Mu_graph;
		M_Mu_graph->GlobalAssemble();

		// Graph block (0,1) of NS
		M_Btranspose_graph.reset (new Epetra_FECrsGraph (Copy, * (M_fespaceUETA->map().map (Unique) ), * (M_fespacePETA->map().map (Unique) ), 0) );
		buildGraph ( elements (M_fespaceUETA->mesh() ),
					 quadRuleTetra4pt,
					 M_fespaceUETA,
					 M_fespacePETA,
					 value(-1.0) * phi_j * div(phi_i)
		) >> M_Btranspose_graph;
		M_Btranspose_graph->GlobalAssemble( *(M_pressureFESpace->map().map (Unique)), *(M_velocityFESpace->map().map (Unique)) );

		// Graph block (1,0) of NS
		M_B_graph.reset (new Epetra_FECrsGraph (Copy, * (M_fespacePETA->map().map (Unique) ), * (M_fespaceUETA->map().map (Unique) ), 0) );
		buildGraph ( elements (M_fespaceUETA->mesh() ),
					 quadRuleTetra4pt,
					 M_fespacePETA,
					 M_fespaceUETA,
					 phi_i * div(phi_j)
		) >> M_B_graph;
		M_B_graph->GlobalAssemble(*(M_velocityFESpace->map().map (Unique)), *(M_pressureFESpace->map().map (Unique)));

		// Graph convective term, block (0,0)
		M_C_graph.reset (new Epetra_FECrsGraph (Copy, * (M_fespaceUETA->map().map (Unique) ), 0) );
		buildGraph ( elements (M_fespaceUETA->mesh() ),
					 quadRuleTetra4pt,
					 M_fespaceUETA,
					 M_fespaceUETA,
					 dot( M_fluidData->density()*value(M_fespaceUETA, *M_uExtrapolated)*grad(phi_j), phi_i)
		) >> M_C_graph;
		M_C_graph->GlobalAssemble();

		// Graph stiffness, block (0,0)
		M_A_graph.reset (new Epetra_FECrsGraph (Copy, * (M_fespaceUETA->map().map (Unique) ), 0) );
		if (M_stiffStrain)
		{
			buildGraph ( elements (M_fespaceUETA->mesh() ),
						 quadRuleTetra4pt,
						 M_fespaceUETA,
						 M_fespaceUETA,
						 value( 0.5 * M_fluidData->viscosity() ) * dot( grad(phi_i) + transpose(grad(phi_i)) , grad(phi_j) + transpose(grad(phi_j)) )
			) >> M_A_graph;
		}
		else
		{
			buildGraph ( elements (M_fespaceUETA->mesh() ),
						 quadRuleTetra4pt,
						 M_fespaceUETA,
						 M_fespaceUETA,
						 M_fluidData->viscosity() * dot( grad(phi_i) , grad(phi_j) + transpose(grad(phi_j)) )
			) >> M_A_graph;
		}
		M_A_graph->GlobalAssemble();

	}

	chrono.stop();
	M_displayer.leaderPrintMax ( "   done in ", chrono.diff() ) ;


	M_displayer.leaderPrint ( " F - Assembling constant terms... ");
	chrono.reset();
	chrono.start();

	{
		using namespace ExpressionAssembly;

		// Graph velocity mass -> block (0,0)
		M_Mu.reset (new matrix_Type ( M_velocityFESpace->map(), *M_Mu_graph ) );
		*M_Mu *= 0;
		integrate ( elements (M_fespaceUETA->mesh() ),
					M_velocityFESpace->qr(),
					M_fespaceUETA,
					M_fespaceUETA,
					M_fluidData->density() * dot ( phi_i, phi_j )
		) >> M_Mu;
		M_Mu->globalAssemble();

		M_Btranspose.reset (new matrix_Type ( M_velocityFESpace->map(), *M_Btranspose_graph ) );
		*M_Btranspose *= 0;
		integrate( elements(M_fespaceUETA->mesh()),
				   M_velocityFESpace->qr(),
				   M_fespaceUETA,
				   M_fespacePETA,
				   value(-1.0) * phi_j * div(phi_i)
		) >> M_Btranspose;
		M_Btranspose->globalAssemble();

		M_B.reset (new matrix_Type ( M_pressureFESpace->map(), *M_B_graph ) );
		*M_B *= 0;
		integrate( elements(M_fespaceUETA->mesh()),
				   M_pressureFESpace->qr(),
				   M_fespacePETA,
				   M_fespaceUETA,
				   phi_i * div(phi_j)
		) >> M_B;
		M_B->globalAssemble();

		M_A.reset (new matrix_Type ( M_velocityFESpace->map(), *M_A_graph ) );
		*M_A *= 0;
		if ( M_stiffStrain )
		{
			integrate( elements(M_fespaceUETA->mesh()),
					   M_velocityFESpace->qr(),
					   M_fespaceUETA,
					   M_fespaceUETA,
					   value( 0.5 * M_fluidData->viscosity() ) * dot( grad(phi_i) + transpose(grad(phi_i)) , grad(phi_j) + transpose(grad(phi_j)) )
			) >> M_A;
		}
		else
		{
			integrate( elements(M_fespaceUETA->mesh()),
					   M_velocityFESpace->qr(),
					   M_fespaceUETA,
				 	   M_fespaceUETA,
					   M_fluidData->viscosity() * dot( grad(phi_i) , grad(phi_j) + transpose(grad(phi_j)) )
			) >> M_A;
		}
		M_A->globalAssemble();
	}

	chrono.stop();
	M_displayer.leaderPrintMax ( " done in ", chrono.diff() ) ;
}

} // namespace LifeV

#endif // NAVIERSTOKESSOLVERBLOCK_H