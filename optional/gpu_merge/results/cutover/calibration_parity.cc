/**
 * \file
 * \author Edward Kmett <ekmett@gmail.com>
 * \brief Generated artifact for the checked optional GPU cutover calibration.
 * SPDX-FileCopyrightText: 2026 Edward Kmett <ekmett@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
 */
#include "calibration.h"
#include <stdexcept>
#include <iostream>
int main() {
using namespace everett_gpu;
auto check=[](bool good){if(!good)throw std::runtime_error("generated calibration parity");};
{
cutover_header a{256ULL,32044ULL,4368ULL,std::nullopt,321ULL}, b{256ULL,32197ULL,4392ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{1024ULL,128124ULL,16464ULL,std::nullopt,321ULL}, b{1024ULL,128211ULL,16480ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,512407ULL,64848ULL,std::nullopt,321ULL}, b{4096ULL,512544ULL,64864ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{16384ULL,2049130ULL,258296ULL,std::nullopt,321ULL}, b{16384ULL,2049259ULL,258312ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,8196298ULL,1032160ULL,std::nullopt,321ULL}, b{65536ULL,8196395ULL,1032168ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,32784814ULL,4127560ULL,std::nullopt,321ULL}, b{262144ULL,32784901ULL,4127576ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,512407ULL,64848ULL,std::nullopt,321ULL}, b{1024ULL,128211ULL,16480ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,512407ULL,64848ULL,std::nullopt,321ULL}, b{256ULL,32197ULL,4392ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,510509ULL,64608ULL,std::nullopt,65ULL}, b{4096ULL,510646ULL,64624ULL,std::nullopt,65ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,515843ULL,65280ULL,std::nullopt,2113ULL}, b{4096ULL,515980ULL,65296ULL,std::nullopt,2113ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,512407ULL,64848ULL,std::nullopt,321ULL}, b{4096ULL,512544ULL,64864ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,512407ULL,64848ULL,std::nullopt,321ULL}, b{4096ULL,512544ULL,64864ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,2199215ULL,275768ULL,std::nullopt,321ULL}, b{4096ULL,2199804ULL,275840ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,15564785ULL,1946560ULL,std::nullopt,321ULL}, b{4096ULL,15568964ULL,1947088ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{4096ULL,356832ULL,45296ULL,72ULL,321ULL}, b{4096ULL,356832ULL,45296ULL,72ULL,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,8196298ULL,1032160ULL,std::nullopt,321ULL}, b{16384ULL,2049259ULL,258312ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,8196298ULL,1032160ULL,std::nullopt,321ULL}, b{4096ULL,512544ULL,64864ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,8169824ULL,1028840ULL,std::nullopt,65ULL}, b{65536ULL,8169921ULL,1028856ULL,std::nullopt,65ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,8224310ULL,1035656ULL,std::nullopt,2113ULL}, b{65536ULL,8224407ULL,1035672ULL,std::nullopt,2113ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,8196298ULL,1032160ULL,std::nullopt,321ULL}, b{65536ULL,8196395ULL,1032168ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,8196298ULL,1032160ULL,std::nullopt,321ULL}, b{65536ULL,8196395ULL,1032168ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,35189720ULL,4407504ULL,std::nullopt,321ULL}, b{65536ULL,35190269ULL,4407568ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,249074740ULL,31144648ULL,std::nullopt,321ULL}, b{65536ULL,249078879ULL,31145168ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,5704726ULL,719040ULL,72ULL,321ULL}, b{65536ULL,5704726ULL,719040ULL,72ULL,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,32784814ULL,4127560ULL,std::nullopt,321ULL}, b{65536ULL,8196395ULL,1032168ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,32784814ULL,4127560ULL,std::nullopt,321ULL}, b{16384ULL,2049259ULL,258312ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,32679698ULL,4114408ULL,std::nullopt,65ULL}, b{262144ULL,32679785ULL,4114416ULL,std::nullopt,65ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,32891468ULL,4140904ULL,std::nullopt,2113ULL}, b{262144ULL,32891555ULL,4140912ULL,std::nullopt,2113ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,32784814ULL,4127560ULL,std::nullopt,321ULL}, b{262144ULL,32784901ULL,4127576ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,32784814ULL,4127560ULL,std::nullopt,321ULL}, b{262144ULL,32784901ULL,4127576ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,140758484ULL,17628944ULL,std::nullopt,321ULL}, b{262144ULL,140759025ULL,17629008ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,507758964ULL,63507944ULL,std::nullopt,321ULL}, b{262144ULL,507761045ULL,63508208ULL,std::nullopt,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{262144ULL,22817996ULL,2875008ULL,72ULL,321ULL}, b{262144ULL,22817996ULL,2875008ULL,72ULL,321ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{65536ULL,270760322ULL,33851024ULL,4116ULL,2113ULL}, b{4096ULL,16924492ULL,2116264ULL,4116ULL,2113ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{2048ULL,619720ULL,78048ULL,std::nullopt,577ULL}, b{2048ULL,620019ULL,78080ULL,std::nullopt,577ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{2048ULL,2162347ULL,270808ULL,1040ULL,1089ULL}, b{682ULL,720805ULL,90504ULL,1040ULL,1089ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{2048ULL,7779242ULL,973048ULL,std::nullopt,65ULL}, b{256ULL,973877ULL,122112ULL,std::nullopt,65ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{2048ULL,181181ULL,23160ULL,72ULL,2113ULL}, b{128ULL,13297ULL,2016ULL,72ULL,2113ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{8192ULL,2478768ULL,311184ULL,std::nullopt,577ULL}, b{8192ULL,2479059ULL,311224ULL,std::nullopt,577ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{8192ULL,8646169ULL,1081808ULL,1040ULL,1089ULL}, b{2730ULL,2882066ULL,360832ULL,1040ULL,1089ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{8192ULL,31129938ULL,3892832ULL,std::nullopt,65ULL}, b{1024ULL,3891653ULL,486952ULL,std::nullopt,65ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{8192ULL,718431ULL,90848ULL,72ULL,2113ULL}, b{512ULL,46879ULL,6248ULL,72ULL,2113ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{32768ULL,9913992ULL,1243568ULL,std::nullopt,577ULL}, b{32768ULL,9914267ULL,1243600ULL,std::nullopt,577ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{32768ULL,34581443ULL,4325832ULL,1040ULL,1089ULL}, b{10922ULL,11527173ULL,1442160ULL,1040ULL,1089ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{32768ULL,124524074ULL,15570824ULL,std::nullopt,65ULL}, b{4096ULL,15567066ULL,1946848ULL,std::nullopt,65ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{32768ULL,2867413ULL,361584ULL,72ULL,2113ULL}, b{2048ULL,181181ULL,23160ULL,72ULL,2113ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == false);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{131072ULL,39654743ULL,4973136ULL,std::nullopt,577ULL}, b{131072ULL,39655066ULL,4973176ULL,std::nullopt,577ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{131072ULL,138322569ULL,17301904ULL,1040ULL,1089ULL}, b{43690ULL,46107538ULL,5767528ULL,1040ULL,1089ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{131072ULL,498096609ULL,62282344ULL,std::nullopt,65ULL}, b{16384ULL,62264081ULL,7785840ULL,std::nullopt,65ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
{
cutover_header a{131072ULL,11463375ULL,1444520ULL,72ULL,2113ULL}, b{8192ULL,718431ULL,90848ULL,72ULL,2113ULL};
check(choose_merge(a,b,calibrated_cutover.identity,calibrated_cutover).gpu == true);
auto wrong=calibrated_cutover.identity; wrong.variant="unmeasured variant";
check(!choose_merge(a,b,wrong,calibrated_cutover).gpu);
}
std::cout << "all50 generated calibration decisions and mismatch fallbacks passed\n";
}
