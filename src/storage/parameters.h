//#ifndef STORM_STORAGE_PARAMETERS_H_
//#define STORM_STORAGE_PARAMETERS_H_

#pragma once

#include "storm-config.h"
#ifdef STORM_HAVE_CARL

#include "../adapters/extendedCarl.h"

namespace storm
{
	typedef carl::MultivariatePolynomial<mpq_class> Polynomial;
	//typedef Parameter  carl::Variable							;
}
#endif

//#endif

