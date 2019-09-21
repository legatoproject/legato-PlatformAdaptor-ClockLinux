//--------------------------------------------------------------------------------------------------
/**
 * Linux Clock Service Adapter
 * Provides adapter for linux specific functionality needed by
 * component
 *
 */
//--------------------------------------------------------------------------------------------------

#include "legato.h"
#include "interfaces.h"
#include <stdlib.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include "pa_clkSync.h"

#define MAX_SYSTEM_CMD_LENGTH 512
#define MAX_SYSTEM_CMD_OUTPUT_LENGTH 1024


//--------------------------------------------------------------------------------------------------
/**
 * Validate a char string as an IPv4/v6 address or not
 *
 * @return
 *      - true    the input char string contains an IPv4/v6 address valid in format
 *      - false   otherwise
 */
//--------------------------------------------------------------------------------------------------
static bool IsIpAddress
(
    const char* addStr  ///< [IN] IP address to classify
)
{
    struct sockaddr_in6 sa;
    if (inet_pton(AF_INET, addStr, &(sa.sin6_addr)) ||
        inet_pton(AF_INET6, addStr, &(sa.sin6_addr)))
    {
        return true;
    }
    return false;
}


//--------------------------------------------------------------------------------------------------
/**
 * Resolve the given host name into an IP address
 *
 * @return
 *      - LE_OK         name resolution into IP addr succeeded
 *      - LE_FAULT      name resolution execution failed or unable to resolve into an IP addr
 */
//--------------------------------------------------------------------------------------------------
static le_result_t ResolveIpAddress
(
    const char* namePtr, ///< [IN] Host name to resolve
    char* ipAddrPtr      ///< [OUT] Resolved IP address in string of length LE_DCS_IPADDR_MAX_LEN
)
{
    int rc;
    struct addrinfo *resultPtr, *nextPtr;
    struct sockaddr_in* serverPtr;
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    rc = getaddrinfo(namePtr, NULL, &hints, &resultPtr);
    if (rc)
    {
        LE_ERROR("getaddrinfo() failed to resolve host name %s with error %s", namePtr,
                 gai_strerror(rc));
        return LE_FAULT;
    }

    for (nextPtr = resultPtr; nextPtr != NULL; nextPtr = nextPtr->ai_next)
    {
        serverPtr = (struct sockaddr_in*)nextPtr->ai_addr;
        inet_ntop(serverPtr->sin_family, &serverPtr->sin_addr, ipAddrPtr, LE_DCS_IPADDR_MAX_LEN);
        LE_DEBUG("Name %s resolved to IP address %s", namePtr, ipAddrPtr);
        freeaddrinfo(resultPtr);
        return LE_OK;
    }

    LE_ERROR("Name %s not resolved to any valid IP address", namePtr);
    freeaddrinfo(resultPtr);
    return LE_FAULT;
}


//--------------------------------------------------------------------------------------------------
/**
 * Retrieve current clock time using the given Linux protocol specific server command in which,
 * and also in the 1st input argument a particular server is specified. The 2nd input argument
 * specifies if this operation is to only get the time or to get it & set it into the system clock.
 * If it is a get only operation, the retrieved current time will be returned in the output and
 * last argument.
 *
 * @return
 *      - LE_OK             Function succeeded to get (if getOnly) or update clock time
 *      - LE_BAD_PARAMETER  Incorrect parameter
 *      - LE_NOT_FOUND      Given server as name or address not found or resolvable into an IP addr
 *      - LE_UNAVAILABLE    No current clock time retrieved from the given server
 *      - LE_FAULT          Function failed to get clock time
 */
//--------------------------------------------------------------------------------------------------
static le_result_t pa_clkSync_GetTimeFromServer
(
    const char* serverStrPtr,        ///< [IN]  Time server name or address
    bool getOnly,                    ///< [IN]  Get the time acquired without updating system clock
    char* protocolCommand,           ///< [IN]  Protocol dependent command to run, i.e. TP or NTP
    le_clkSync_ClockTime_t* timePtr  ///< [OUT] Time structure
)
{
    le_result_t result;
    FILE* fp;
    char output[MAX_SYSTEM_CMD_OUTPUT_LENGTH], serverIpAddrStr[LE_DCS_IPADDR_MAX_LEN] = {};

    if (!timePtr)
    {
        // Not supposed to happen
        LE_ERROR("Null time data structure");
        return LE_BAD_PARAMETER;
    }

    memset(timePtr, 0, sizeof(le_clkSync_ClockTime_t));
    if ((!serverStrPtr) || ('\0' == serverStrPtr[0]))
    {
        LE_ERROR("Incorrect parameter");
        return LE_BAD_PARAMETER;
    }

    // Validate time server name resolution if given as a name
    if (!IsIpAddress(serverStrPtr))
    {
        result = ResolveIpAddress(serverStrPtr, serverIpAddrStr);
        if (result != LE_OK)
        {
            LE_WARN("Failed to resolve server %s into IP address to get clock time",
                    serverStrPtr);
            return LE_NOT_FOUND;
        }
    }

    if (getOnly)
    {
        fp = popen(protocolCommand, "r");
        if (!fp)
        {
            LE_ERROR("Failed to run command '%s' (%m)", protocolCommand);
            return LE_FAULT;
        }

        // Retrieve output
        struct tm tm = {0};
        result = LE_FAULT;
        while ((NULL != fgets(output, sizeof(output)-1, fp)) && (LE_OK != result))
        {
            if (NULL != strptime(output, "%a %b %d %H:%M:%S %Y", &tm))
            {
                timePtr->msec = 0;
                timePtr->sec  = tm.tm_sec;
                timePtr->min  = tm.tm_min;
                timePtr->hour = tm.tm_hour;
                timePtr->day  = tm.tm_mday;
                timePtr->mon  = 1 + tm.tm_mon; // Convert month range to [1..12]
                timePtr->year = 1900 + tm.tm_year;
                result = LE_OK;
            }
        }
    }
    else
    {
        fp = popen(protocolCommand, "r");
        if (!fp)
        {
            LE_ERROR("Failed to run command '%s' (%m)", protocolCommand);
            return LE_FAULT;
        }

        result = LE_UNAVAILABLE;
        while (fgets(output, sizeof(output)-1, fp))
        {
            if (strlen(output) > 1)
            {
                char *tmp_ptr = NULL;
                int16_t resultCode = (int16_t)strtol(output, &tmp_ptr, 10);
                LE_INFO("Result: %d", resultCode);
                result = (resultCode == 0) ? LE_OK : LE_FAULT;
                break;
            }
        }
    }
    pclose(fp);
    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Retrieve time from a server using the Time Protocol.
 *
 * @return
 *      - LE_OK             Function succeeded to get (if getOnly) or update clock time
 *      - LE_BAD_PARAMETER  Incorrect parameter
 *      - LE_NOT_FOUND      Given server as name or address not found or resolvable into an IP addr
 *      - LE_UNAVAILABLE    No current clock time retrieved from the given server
 *      - LE_UNSUPPORTED    Function not supported by the target
 *      - LE_FAULT          Function failed to get clock time
 */
//--------------------------------------------------------------------------------------------------
le_result_t pa_clkSync_GetTimeWithTimeProtocol
(
    const char* serverStrPtr,       ///< [IN]  Time server
    bool getOnly,                   ///< [IN]  Get the time acquired without updating system clock
    le_clkSync_ClockTime_t* timePtr ///< [OUT] Time structure
)
{
    le_result_t result;
    char protocolCommand[MAX_SYSTEM_CMD_LENGTH] = {0};

    if (getOnly)
    {
        snprintf(protocolCommand, sizeof(protocolCommand), "/usr/sbin/rdate -p %s", serverStrPtr);
    }
    else
    {
        snprintf(protocolCommand, sizeof(protocolCommand),
                 "/usr/sbin/rdate %s >& /dev/null; echo $?", serverStrPtr);
    }

    result = pa_clkSync_GetTimeFromServer(serverStrPtr, getOnly, protocolCommand, timePtr);
    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Retrieve time from a server using the Network Time Protocol.
 *
 * @return
 *      - LE_OK             Function succeeded to get (if getOnly) or update clock time
 *      - LE_BAD_PARAMETER  Incorrect parameter
 *      - LE_NOT_FOUND      Given server as name or address not found or resolvable into an IP addr
 *      - LE_UNAVAILABLE    No current clock time retrieved from the given server
 *      - LE_UNSUPPORTED    Function not supported by the target
 *      - LE_FAULT          Function failed to get clock time
 */
//--------------------------------------------------------------------------------------------------
le_result_t pa_clkSync_GetTimeWithNetworkTimeProtocol
(
    const char* serverStrPtr,       ///< [IN]  Time server
    bool getOnly,                   ///< [IN]  Get the time acquired without updating system clock
    le_clkSync_ClockTime_t* timePtr ///< [OUT] Time structure
)
{
    le_result_t result;
    char protocolCommand[MAX_SYSTEM_CMD_LENGTH] = {0};

    if (getOnly)
    {
        memset(timePtr, 0, sizeof(le_clkSync_ClockTime_t));
        LE_ERROR("Unsupported to only get clock time from NTP without setting it into system");
        return LE_UNSUPPORTED;
    }
    else
    {
        snprintf(protocolCommand, sizeof(protocolCommand),
                 "/usr/sbin/ntpd -n -q -p %s >& /dev/null; echo $?", serverStrPtr);
    }

    result = pa_clkSync_GetTimeFromServer(serverStrPtr, getOnly, protocolCommand, timePtr);
    return result;
}


//--------------------------------------------------------------------------------------------------
/**
 * Component init
 */
//--------------------------------------------------------------------------------------------------
COMPONENT_INIT
{
}
