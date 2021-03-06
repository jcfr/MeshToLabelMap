/*=========================================================================

  Program:   Visualization Toolkit
  Module:    $RCSfile: vtkPolyDataToImageStencilOBBTree.cxx,v $

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#ifndef __vtkPolyDataToImageStencilOBBTree_cxx
#define __vtkPolyDataToImageStencilOBBTree_cxx

#include "vtkPolyDataToImageStencilOBBTree.h"

#include "vtkGarbageCollector.h"
#include "vtkImageStencilData.h"
#include "vtkImageData.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkOBBTree.h"
#include "vtkObjectFactory.h"
#include "vtkPolyData.h"
#include "vtkStreamingDemandDrivenPipeline.h"

#include <math.h>

vtkStandardNewMacro(vtkPolyDataToImageStencilOBBTree);
vtkCxxSetObjectMacro(vtkPolyDataToImageStencilOBBTree, InformationInput,
                     vtkImageData);

//----------------------------------------------------------------------------
vtkPolyDataToImageStencilOBBTree::vtkPolyDataToImageStencilOBBTree()
{
  this->OBBTree = NULL;
  this->Tolerance = 1e-3;

  // InformationInput is an input used only for its information.
  // The vtkImageStencilSource produces a structured data
  // set with a specific "Spacing" and "Origin", and the
  // InformationInput is a source of this information.
  this->InformationInput = NULL;

  // If no InformationInput is set, then the Spacing and Origin
  // for the output must be set directly.
  this->OutputOrigin[0] = 0;
  this->OutputOrigin[1] = 0;
  this->OutputOrigin[2] = 0;
  this->OutputSpacing[0] = 1;
  this->OutputSpacing[1] = 1;
  this->OutputSpacing[2] = 1;

  // The default output extent is essentially infinite, which allows
  // this filter to produce any requested size.  This would not be a
  // great source to connect to some sort of writer or viewer, it
  // should only be connected to multiple-input filters that take
  // compute their output extent from one of the other inputs.
  this->OutputWholeExtent[0] = 0;
  this->OutputWholeExtent[1] = VTK_INT_MAX >> 2;
  this->OutputWholeExtent[2] = 0;
  this->OutputWholeExtent[3] = VTK_INT_MAX >> 2;
  this->OutputWholeExtent[4] = 0;
  this->OutputWholeExtent[5] = VTK_INT_MAX >> 2;
}

//----------------------------------------------------------------------------
vtkPolyDataToImageStencilOBBTree::~vtkPolyDataToImageStencilOBBTree()
{
  this->SetInformationInput(NULL);

  if (this->OBBTree)
    {
      this->OBBTree->Delete();
    }
  if ( this->GetInput () )
    {
      this->GetInput()->Delete() ;
    }
}

//----------------------------------------------------------------------------
void vtkPolyDataToImageStencilOBBTree::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

    os << indent << "InformationInput: " << this->InformationInput << "\n";
  os << indent << "OutputSpacing: " << this->OutputSpacing[0] << " " <<
    this->OutputSpacing[1] << " " << this->OutputSpacing[2] << "\n";
  os << indent << "OutputOrigin: " << this->OutputOrigin[0] << " " <<
    this->OutputOrigin[1] << " " << this->OutputOrigin[2] << "\n";
  os << indent << "OutputWholeExtent: " << this->OutputWholeExtent[0] << " " <<
    this->OutputWholeExtent[1] << " " << this->OutputWholeExtent[2] << " " <<
    this->OutputWholeExtent[3] << " " << this->OutputWholeExtent[4] << " " <<
    this->OutputWholeExtent[5] << "\n";
  os << indent << "Input: " << this->GetInput() << "\n";
  os << indent << "Tolerance: " << this->Tolerance << "\n";
}

//----------------------------------------------------------------------------
void vtkPolyDataToImageStencilOBBTree::SetInput(vtkPolyData *input)
{
  if (input)
    {
    this->SetInputDataObject(0, input);
    }
  else
    {
    this->SetInputDataObject(0, 0);
    }
}

//----------------------------------------------------------------------------
vtkPolyData *vtkPolyDataToImageStencilOBBTree::GetInput()
{
  if (this->GetNumberOfInputConnections(0) < 1)
    {
    return NULL;
    }
  
  return vtkPolyData::SafeDownCast(
    this->GetExecutive()->GetInputData(0, 0));
}

//----------------------------------------------------------------------------
void vtkAddEntryToList(int *&clist, int &clistlen, int &clistmaxlen, int r)
{
  if (clistlen >= clistmaxlen)
    { // need to allocate more space
    clistmaxlen *= 2;
    int *newclist = new int[clistmaxlen];
    for (int k = 0; k < clistlen; k++)
      {
      newclist[k] = clist[k];
      }
    delete [] clist;
    clist = newclist;
    }

  if (clistlen > 0 && r <= clist[clistlen-1])
    { // chop out zero-length extents
    clistlen--;
    }
  else
    {
    clist[clistlen++] = r;
    }
}

//----------------------------------------------------------------------------
void vtkTurnPointsIntoList(vtkSmartPointer<vtkPoints> points, int *&clist, int &clistlen,
                           int extent[6], double origin[3], double spacing[3],
                           int dim)
{
  int clistmaxlen = 2;
  clistlen = 0;
  clist = new int[clistmaxlen];

  int npoints = points->GetNumberOfPoints();
  for (int idP = 0; idP < npoints; idP++)
    {
    double point[3];
    points->GetPoint(idP, point);
    int r = (int)ceil((point[dim] - origin[dim])/spacing[dim]);
    if (r < extent[2*dim])
      {
      r = extent[2*dim];
      }
    if (r > extent[2*dim+1])
      {
      break;
      }
    vtkAddEntryToList(clist, clistlen, clistmaxlen, r);
    }
}

//----------------------------------------------------------------------------
int vtkPolyDataToImageStencilOBBTree::RequestInformation(
  vtkInformation *,
  vtkInformationVector **,
  vtkInformationVector *outputVector)
{
  vtkInformation *outInfo = outputVector->GetInformationObject(0);
  
  int wholeExtent[6];
  double spacing[3];
  double origin[3];

  for (int i = 0; i < 3; i++)
    {
    wholeExtent[2*i] = this->OutputWholeExtent[2*i];
    wholeExtent[2*i+1] = this->OutputWholeExtent[2*i+1];
    spacing[i] = this->OutputSpacing[i];
    origin[i] = this->OutputOrigin[i];
    }

  // If InformationInput is set, then get the spacing,
  // origin, and whole extent from it.
  if (this->InformationInput)
    {
    this->InformationInput->GetExtent(wholeExtent);
    this->InformationInput->GetSpacing(spacing);
    this->InformationInput->GetOrigin(origin);
    }

  outInfo->Set(vtkStreamingDemandDrivenPipeline::WHOLE_EXTENT(),
               wholeExtent, 6);
  outInfo->Set(vtkDataObject::SPACING(), spacing, 3);
  outInfo->Set(vtkDataObject::ORIGIN(), origin, 3);

  return 1;
}

//----------------------------------------------------------------------------
int vtkPolyDataToImageStencilOBBTree::RequestData(
  vtkInformation *request,
  vtkInformationVector **inputVector,
  vtkInformationVector *outputVector)
{
  this->Superclass::RequestData(request, inputVector, outputVector);

  vtkInformation *inInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation *outInfo = outputVector->GetInformationObject(0);

  // need to build the OBB tree
  vtkPolyData *polydata = vtkPolyData::SafeDownCast(
    inInfo->Get(vtkDataObject::DATA_OBJECT()));
  vtkImageStencilData *data = vtkImageStencilData::SafeDownCast(
    outInfo->Get(vtkDataObject::DATA_OBJECT()));

  if (this->OBBTree == NULL)
    {
    this->OBBTree = vtkOBBTree::New();
    }
  this->OBBTree->SetDataSet(polydata);
  this->OBBTree->SetTolerance(this->Tolerance);
  this->OBBTree->BuildLocator();

  // for keeping track of progress
  unsigned long count = 0;
  int extent[6];
  data->GetExtent(extent);
  unsigned long target = (unsigned long)
    ((extent[5] - extent[4] + 1)*(extent[3] - extent[2] + 1)/50.0);
  target++;

  // if we have no data then return
  if (!polydata->GetNumberOfPoints())
    {
    return 1;
    }
  
  double *spacing = data->GetSpacing();
  double *origin = data->GetOrigin();

  vtkOBBTree *tree = this->OBBTree;
  vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();

  double p0[3],p1[3];

  p1[0] = p0[0] = extent[0]*spacing[0] + origin[0];
  p1[1] = p0[1] = extent[2]*spacing[1] + origin[1];
  p0[2] = extent[4]*spacing[2] + origin[2];
  p1[2] = extent[5]*spacing[2] + origin[2];

  int zstate = tree->InsideOrOutside(p0);
  if (zstate == 0)
    {
    zstate = -1;
    }
  int *zlist = 0;
  int zlistlen = 0;
  int zlistidx = 0;
  if (extent[4] < extent[5])
    {
    tree->IntersectWithLine(p0, p1, points, 0);
    vtkTurnPointsIntoList(points, zlist, zlistlen,
                          extent, origin, spacing, 2);
    }

  for (int idZ = extent[4]; idZ <= extent[5]; idZ++)
    {
    if (zlistidx < zlistlen && idZ >= zlist[zlistidx])
      {
      zstate = -zstate;
      zlistidx++;
      }

    p1[0] = p0[0] = extent[0]*spacing[0] + origin[0];
    p0[1] = extent[2]*spacing[1] + origin[1];
    p1[1] = extent[3]*spacing[1] + origin[1];
    p1[2] = p0[2] = idZ*spacing[2] + origin[2];

    int ystate = zstate;
    int *ylist = 0;
    int ylistlen = 0;
    int ylistidx = 0;
    if (extent[2] != extent[3])
      {
      tree->IntersectWithLine(p0, p1, points, 0);
      vtkTurnPointsIntoList(points, ylist, ylistlen,
                            extent, origin, spacing, 1);
      }

    for (int idY = extent[2]; idY <= extent[3]; idY++)
      {
      if (ylistidx < ylistlen && idY >= ylist[ylistidx])
        {
        ystate = -ystate;
        ylistidx++;
        }

      if (count%target == 0) 
        {
        this->UpdateProgress(count/(50.0*target));
        }
      count++;

      p0[1] = p1[1] = idY*spacing[1] + origin[1];
      p0[2] = p1[2] = idZ*spacing[2] + origin[2];
      p0[0] = extent[0]*spacing[0] + origin[0];
      p1[0] = extent[1]*spacing[0] + origin[0];

      int xstate = ystate;
      int *xlist = 0;
      int xlistlen = 0;
      int xlistidx = 0;
      tree->IntersectWithLine(p0, p1, points, 0);
      vtkTurnPointsIntoList(points, xlist, xlistlen,
                            extent, origin, spacing, 0);

      // now turn 'xlist' into sub-extents:
      int r1 = extent[0];
      int r2 = extent[1];
      for (xlistidx = 0; xlistidx < xlistlen; xlistidx++)
        {
        xstate = -xstate;
        if (xstate < 0)
          { // sub extent starts
          r1 = xlist[xlistidx];
          }
        else
          { // sub extent ends
          r2 = xlist[xlistidx] - 1;
          data->InsertNextExtent(r1, r2, idY, idZ);
          }
        }
      if (xstate < 0)
        { // if inside at end, cap off the sub extent
        data->InsertNextExtent(r1, extent[1], idY, idZ);
        }      

      if (xlist)
        {
        delete [] xlist;
        }

      } // for idY

    if (ylist)
      {
      delete [] ylist;
      }

    } // for idZ

  if (zlist)
    {
    delete [] zlist;
    }
  return 1;
}

//----------------------------------------------------------------------------
void vtkPolyDataToImageStencilOBBTree::ReportReferences(vtkGarbageCollector* collector)
{
  this->Superclass::ReportReferences(collector);
  // This filter shares our input and is therefore involved in a
  // reference loop.
  vtkGarbageCollectorReport(collector, this->OBBTree, "OBBTree");
}

//----------------------------------------------------------------------------
int vtkPolyDataToImageStencilOBBTree::FillInputPortInformation(int,
                                                        vtkInformation* info)
{
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkPolyData");
  return 1;
}

#endif
